#include <vector>
#include <functional>

#include <lf/assemble/assemble.h>
#include <lf/base/base.h>
#include <lf/io/io.h>
#include <lf/mesh/test_utils/test_meshes.h>
#include <lf/mesh/utils/utils.h>
#include <lf/refinement/refinement.h>
#include <lf/uscalfe/uscalfe.h>

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include "../ExtendPum/ExtendPUM_EdgeMat.h"
#include "../ExtendPum/ExtendPUM_EdgeVector.h"
#include "../ExtendPum/ExtendPUM_ElementMatrix.h"
#include "../ExtendPum/ExtendPUM_ElemVector.h"
#include "../utils/HE_solution.h"
#include "../utils/utils.h"

using namespace std::complex_literals;
using size_type = unsigned int;
using Scalar = std::complex<double>;
using coordinate_t = Eigen::Vector2d;
using Mat_t = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
using triplet_t = Eigen::Triplet<Scalar>;
using SpMat_t = Eigen::SparseMatrix<Scalar>;
using Vec_t = Eigen::Matrix<Scalar, Eigen::Dynamic, 1>;
using FHandle_t = std::function<Scalar(const Eigen::Vector2d &)>;
using FunGradient_t = std::function<Eigen::Matrix<Scalar, 2, 1>(const coordinate_t&)>;
/*
 * Solve the Laplace equation 
 *  \Laplace u = 0 in \Omega
 *   u = g on \partial \Omega
 */
class Laplace {
public:
    Laplace(int levels, const std::string& mesh_path, FHandle_t g, std::vector<int> nr_planwave, int degree=20): 
        L_(levels), g_(g), num_planwaves_(nr_planwave), degree_(degree){
        auto mesh_factory = std::make_unique<lf::mesh::hybrid2d::MeshFactory>(2);
        reader_ = std::make_shared<lf::io::GmshReader>(std::move(mesh_factory), mesh_path);
        mesh_hierarchy_ = lf::refinement::GenerateMeshHierarchyByUniformRefinemnt(reader_->mesh(), L_);
    }

    std::shared_ptr<lf::mesh::Mesh> getmesh(size_type l) { return mesh_hierarchy_->getMesh(l); }

    lf::assemble::UniformFEDofHandler get_dofh(size_type l) {
        int dofs_perNode;
        if(l == L_) {
            dofs_perNode = 1;
        } else {
            dofs_perNode = num_planwaves_[l] + 1;
        }
        return lf::assemble::UniformFEDofHandler(getmesh(l), {{lf::base::RefEl::kPoint(), 1}});
    }

    int nr_dofs(int l) {
        auto dofh = get_dofh(l);
        return dofh.NumDofs();
    }

    std::pair<SpMat_t, Vec_t> build_equation(size_type l) {
        
        auto mesh = getmesh(l);  // get mesh
        auto fe_space = std::make_shared<lf::uscalfe::FeSpaceLagrangeO1<double>>(mesh);
        
        auto dofh = lf::assemble::UniformFEDofHandler(mesh, {{lf::base::RefEl::kPoint(), 1}});
        
        // assemble for <grad(u), grad(v)> 
        lf::mesh::utils::MeshFunctionConstant<double> mf_identity(1.);
        lf::mesh::utils::MeshFunctionConstant<double> mf_zero(0.0);
        lf::uscalfe::ReactionDiffusionElementMatrixProvider<double, decltype(mf_identity), decltype(mf_zero)> 
            elmat_builder(fe_space, mf_identity, mf_zero);
        
        size_type N_dofs(dofh.NumDofs());
        lf::assemble::COOMatrix<Scalar> A(N_dofs, N_dofs);
        lf::assemble::AssembleMatrixLocally(0, dofh, dofh, elmat_builder, A);
        
        // Assemble RHS vector
        Vec_t phi(N_dofs);
        phi.setZero();
        
        auto boundary_point{lf::mesh::utils::flagEntitiesOnBoundary(mesh, 2)};
        std::vector<std::pair<bool, Scalar>> ess_dof_select{};
        for(size_type dofnum = 0; dofnum < N_dofs; ++dofnum) {
            const lf::mesh::Entity &dof_node{dofh.Entity(dofnum)};
            const Eigen::Vector2d node_pos{lf::geometry::Corners(*dof_node.Geometry()).col(0)};
            const Scalar h_val = g_(node_pos);
            if(boundary_point(dof_node)) {
                ess_dof_select.push_back({true, h_val});
            } else {
                ess_dof_select.push_back({false, h_val});
            }
        }

        // modify linear system of equations
        lf::assemble::FixFlaggedSolutionCompAlt<Scalar>(
            [&ess_dof_select](size_type dof_idx)->std::pair<bool, Scalar> {
                return ess_dof_select[dof_idx];},
        A, phi);
    
        return std::make_pair(A.makeSparse(), phi);
    }

    SpMat_t prolongation_lagrange(int l){
        LF_ASSERT_MSG(l >= 0 && l < L_, "l in prolongation should be smaller to L");
        auto coarse_mesh = getmesh(l);
        auto fine_mesh   = getmesh(l+1);
        
        auto coarse_dofh = lf::assemble::UniformFEDofHandler(coarse_mesh, 
                            {{lf::base::RefEl::kPoint(), 1}});
        auto fine_dof    = lf::assemble::UniformFEDofHandler(fine_mesh, 
                            {{lf::base::RefEl::kPoint(), 1}});

        size_type n_c = coarse_dofh.NumDofs();
        size_type n_f = fine_dof.NumDofs();
        
        Mat_t M = Mat_t::Zero(n_c, n_f);

        for(const lf::mesh::Entity* edge: fine_mesh->Entities(1)) {
            nonstd::span<const lf::mesh::Entity* const> points = edge->SubEntities(1);
            size_type num_points = (*edge).RefEl().NumSubEntities(1); // number of endpoints, should be 2
            LF_ASSERT_MSG((num_points == 2), 
                "Every EDGE should have 2 kPoint subentities");
            for(int j = 0; j < num_points; ++j) {
                auto parent_p = mesh_hierarchy_->ParentEntity(l+1, *points[j]); // parent entity of current point 
                if(parent_p->RefEl() == lf::base::RefEl::kPoint()) {
                    // it's parent is also a NODE. If the point in finer mesh does not show in coarser mesh,
                    // then it's parent is an EDGE
                    M(coarse_mesh->Index(*parent_p), fine_mesh->Index(*points[j])) = 1.0;
                    M(coarse_mesh->Index(*parent_p), fine_mesh->Index(*points[1-j])) = 0.5;
                }
            }
        }
        return (M.transpose()).sparseView();
    }

    SpMat_t prolongation(int l) {
        double k = 5.0;
        if(l == L_ - 1){
            double pi = std::acos(-1.);
            auto Q = prolongation_lagrange(L_ - 1);
            int n = Q.rows();
            int N = num_planwaves_[L_-1];

            Mat_t E = Mat_t::Zero(n, N+1);

            auto S_mesh = getmesh(L_);
            auto S_dofh = lf::assemble::UniformFEDofHandler(S_mesh, 
                                {{lf::base::RefEl::kPoint(), 1}});
            for(int i = 0; i < n; ++i) {
                const lf::mesh::Entity& vi = S_dofh.Entity(i); // the entity to which i-th global shape function is associated
                coordinate_t vi_coordinate = lf::geometry::Corners(*vi.Geometry()).col(0);
                E(i,0) = 1.0;
                for(int t = 1; t <= N; ++t) {
                    Eigen::Vector2d dt;
                    dt << std::cos(2*pi*(t-1)/N), std::sin(2*pi*(t-1)/N);
                    E(i,t) = std::exp(1i*k*dt.dot(vi_coordinate));
                }
            }

            SpMat_t res(n, Q.cols() * (N+1));
            std::vector<triplet_t> triplets;
            for(int outer_idx = 0; outer_idx < Q.outerSize(); ++outer_idx) {
                for(SpMat_t::InnerIterator it(Q, outer_idx); it; ++it) {
                    int i = it.row();
                    int j = it.col();
                    Scalar qij = it.value(); 
                    for(int k = 0; k <= N; ++k) {
                        triplets.push_back(triplet_t(i, j*(N+1)+k, qij*E(i,k)));
                    }
                }
            }
            res.setFromTriplets(triplets.begin(), triplets.end());
            return res;
        } else {
            double pi = std::acos(-1.);
            auto Q = prolongation_lagrange(l);
            int n1 = Q.cols(), n2 = Q.rows(); // n1: n_l, n2: n_{l+1}
            int N1 = num_planwaves_[l], N2 = num_planwaves_[l+1]; // N1: N_l, N2: N_{l+1}

            auto mesh = getmesh(l+1);  // fine mesh
            auto dofh = lf::assemble::UniformFEDofHandler(mesh, {{lf::base::RefEl::kPoint(), 1}});

            SpMat_t res(n2*(N2+1), n1*(N1+1)); // transfer operator
            std::vector<triplet_t> triplets;
            
            for(int outer_idx = 0; outer_idx < Q.outerSize(); ++outer_idx) {
                for(SpMat_t::InnerIterator it(Q, outer_idx); it; ++it) {
                    int i = it.row();
                    int j = it.col();
                    Scalar qij = it.value(); 
                    
                    // b_j^l = \sum_i qij b_i^{l+1} (t = 0)
                    triplets.push_back(triplet_t(i*(N2+1), j*(N1+1), qij));

                    // b_j^l e_{2t-1}^l = \sum_i qij b_i^{l+1} e_t^{l+1}
                    for(int t = 1; t <= N2; ++t) {
                        triplets.push_back(triplet_t(i*(N2+1)+t, j*(N1+1)+2*t-1, qij));
                    } 

                    const lf::mesh::Entity& p_i = dofh.Entity(i); // the entity to which i-th global shape function is associated
                    coordinate_t pi_coordinate = lf::geometry::Corners(*p_i.Geometry()).col(0);

                    for(int t = 1; t <= N2; ++t) {
                        Eigen::Vector2d d1, d2;
                        d1 << std::cos(2*pi*(2*t-1)/N1), std::sin(2*pi*(2*t-1)/N1); // d_{2t}^l
                        d2 << std::cos(2*pi*(  t-1)/N2), std::sin(2*pi*(  t-1)/N2); // d_{t}^{l+1}
                        Scalar tmp = qij * std::exp(1i*k*(d1-d2).dot(pi_coordinate));
                        triplets.push_back(triplet_t(i*(N2+1)+t, j*(N1+1)+2*t, tmp));
                    }
                }
            }
            res.setFromTriplets(triplets.begin(), triplets.end());
            return res;
        }
    }

    Vec_t solve(size_type l) {
        auto eq_pair = build_equation(l);
        Eigen::SparseLU<SpMat_t> solver;
        solver.compute(eq_pair.first);
        Vec_t fe_sol;
        if(solver.info() == Eigen::Success) {
            fe_sol = solver.solve(eq_pair.second);
        } else {
            LF_ASSERT_MSG(false, "Eigen Factorization failed");
        }
        return fe_sol;
    }

    std::vector<double> mesh_width() {
        std::vector<double> width(L_+1); 
        auto mesh = getmesh(0);
        double h0 = 0.0;

        for(const lf::mesh::Entity* cell: mesh->Entities(0)) {
            const lf::geometry::Geometry *geo_ptr = cell->Geometry();

            // coordiante matrix: 2x3
            auto vertices = geo_ptr->Global(cell->RefEl().NodeCoords());
            for(int i = 0; i < vertices.cols(); ++i) {
                for(int j = i + 1; j < vertices.cols(); ++j) {
                    double tmp = (vertices.col(i) - vertices.col(j)).norm();
                    if(tmp > h0) {
                        h0 = tmp;
                    }
                }
            }
        }
        width[0] = h0;
        for(int i = 1; i <= L_; ++i) {
            // because meshes are generated by uniform regular refinement
            width[i] = width[i-1] / 2;
        }
        return width;
    }

    double L2_Err(size_type l, const Vec_t& mu, const FHandle_t& u){
        auto mesh = getmesh(l);
        auto fe_space = std::make_shared<lf::uscalfe::FeSpaceLagrangeO1<double>>(mesh);
        double res = 0.0;

        auto dofh = lf::assemble::UniformFEDofHandler(mesh, {{lf::base::RefEl::kPoint(), 1}});

        for(const lf::mesh::Entity* cell: mesh->Entities(0)) {
            const lf::geometry::Geometry *geo_ptr = cell->Geometry();
        
            auto vertices = geo_ptr->Global(cell->RefEl().NodeCoords());
            Eigen::Matrix3d X, tmp;
            tmp.block<3,1>(0,0) = Eigen::Vector3d::Ones();
            tmp.block<3,2>(0,1) = vertices.transpose();
            X = tmp.inverse();

            const lf::assemble::size_type no_dofs(dofh.NumLocalDofs(*cell));
            nonstd::span<const lf::assemble::gdof_idx_t> dofarray{dofh.GlobalDofIndices(*cell)};

            auto integrand = [&X, &mu, &dofarray, &u](const Eigen::Vector2d& x)->Scalar {
                Scalar uh = 0.0;
                for(int i = 0; i < 3; ++i) {
                    uh += mu(dofarray[i]) * (X(0,i) + x.dot(X.block<2,1>(1,i)));
                }
                return std::abs((uh - u(x)) * (uh - u(x)));
            };
            res += std::abs(LocalIntegral(*cell, degree_, integrand));
        }
        return std::sqrt(res);
    }

    double H1_semiErr(size_type l, const Vec_t& mu, const FunGradient_t& grad_u) {
        auto mesh = getmesh(l);
        auto fe_space = std::make_shared<lf::uscalfe::FeSpaceLagrangeO1<double>>(mesh);
        double res = 0.0;

        auto dofh = lf::assemble::UniformFEDofHandler(mesh, {{lf::base::RefEl::kPoint(), 1}});

        for(const lf::mesh::Entity* cell: mesh->Entities(0)) {
            const lf::geometry::Geometry *geo_ptr = cell->Geometry();
        
            auto vertices = geo_ptr->Global(cell->RefEl().NodeCoords());
            Eigen::Matrix3d X, tmp;
            tmp.block<3,1>(0,0) = Eigen::Vector3d::Ones();
            tmp.block<3,2>(0,1) = vertices.transpose();
            X = tmp.inverse();

            const lf::assemble::size_type no_dofs(dofh.NumLocalDofs(*cell));
            nonstd::span<const lf::assemble::gdof_idx_t> dofarray{dofh.GlobalDofIndices(*cell)};

            auto grad_uh = (Eigen::Matrix<Scalar,2,1>() << 0.0, 0.0).finished();
            for(size_type i = 0; i < no_dofs; ++i) {
                grad_uh += mu(dofarray[i]) * X.block<2,1>(1,i);
            }
            // construct ||grad uh - grad u||^2_{cell}
            auto integrand = [&grad_uh, &grad_u](const Eigen::Vector2d& x)->Scalar {
                return std::abs((grad_uh - grad_u(x)).dot(grad_uh - grad_u(x)));
            };
            res += std::abs(LocalIntegral(*cell, degree_, integrand));
        }
        return std::sqrt(res);
    }

    void solve_multigrid(Vec_t& initial, size_type start_layer, int num_coarserlayer, int mu1, int mu2, bool solve_coarest=true) {
        LF_ASSERT_MSG((num_coarserlayer <= start_layer), 
            "please use a smaller number of wave layers");
        auto eq_pair = build_equation(start_layer);
        SpMat_t A = eq_pair.first;

        std::vector<SpMat_t> Op(num_coarserlayer + 1), prolongation_op(num_coarserlayer);
        std::vector<int> stride(num_coarserlayer + 1, 1);
        Op[num_coarserlayer] = A;
        for(int i = num_coarserlayer - 1; i >= 0; --i) {
            int idx = start_layer + i - num_coarserlayer;
            prolongation_op[i] = prolongation(idx);
            Op[i] = prolongation_op[i].transpose() * Op[i+1] * prolongation_op[i];
            stride[i] = num_planwaves_[idx] + 1;
        }

        v_cycle(initial, eq_pair.second, Op, prolongation_op, stride, mu1, mu2, solve_coarest);

        // power iteration
        // int N = initial.size();
        // Vec_t u = Vec_t::Random(N);
        // u.normalize();
        // Vec_t old_u;
        // Vec_t zero_vec = Vec_t::Zero(N);
        // Scalar lambda;
        // int cnt = 0;
        
        // std::cout << std::left << std::setw(10) << "Iteration" 
        //     << std::setw(20) << "residual_norm" << std::endl;
        // while(true) {
        //     cnt++;
        //     old_u = u;
        //     v_cycle(u, zero_vec, Op, prolongation_op, stride, mu1, mu2, solve_coarest);
            
        //     lambda = old_u.dot(u);  // domainant eigenvalue
        //     auto r = u - lambda * old_u;
            
        //     u.normalize();
        
        //     if(cnt % 1 == 0) {
        //         std::cout << std::left << std::setw(10) << cnt 
        //             << std::setw(20) << r.norm() 
        //             << std::setw(20) << (u - old_u).norm()
        //             << std::endl;
        //     }
        //     if(r.norm() < 0.1) {
        //         break;
        //     }
        //     if(cnt > 20) {
        //         std::cout << "Power iteration for multigrid doesn't converge." << std::endl;
        //         break;
        //     }
        // }
        // std::cout << "Number of iterations: " << cnt << std::endl;
        // std::cout << "Domainant eigenvalue by power iteration: " << lambda << std::endl;
        // return std::make_pair(u, lambda);
    }
private:
    int L_; // number of refinement steps

    std::shared_ptr<lf::io::GmshReader> reader_; // read the coarest mesh
    std::shared_ptr<lf::refinement::MeshHierarchy> mesh_hierarchy_;
    int degree_;  // quad degree
    FHandle_t g_;
    std::vector<int> num_planwaves_;  // number of plan waves per mesh
};

void Laplace_mg(Laplace& laplace, int L, int nr_coarsemesh, FHandle_t u) {
    auto zero_fun = [](const coordinate_t& x) -> Scalar { return 0.0; };

    int N = laplace.nr_dofs(L);
    Vec_t v = Vec_t::Random(N); // initial value
    Vec_t uh = laplace.solve(L);

    int nu1 = 3, nu2 = 3;

    std::vector<double> L2_vk;
    std::vector<double> L2_ek;

    // std::cout << std::scientific << std::setprecision(1);
    for(int k = 0; k < 10; ++k) {
        std::cout << laplace.L2_Err(L, v - uh, zero_fun) << " ";
        std::cout << laplace.L2_Err(L, v, u) << std::endl;
        L2_vk.push_back(laplace.L2_Err(L, v, zero_fun));
        L2_ek.push_back(laplace.L2_Err(L, v - uh, zero_fun));
        laplace.solve_multigrid(v, L, nr_coarsemesh, nu1, nu2, false);
    }
    std::cout << "||u-uh||_2 = " << laplace.L2_Err(L, uh, u) << std::endl;
    std::cout << "||v_{k+1}||/||v_k||" << std::endl;
    for(int k = 0; k + 1 < L2_vk.size(); ++k) {
        std::cout << k << " " << L2_vk[k+1] / L2_vk[k] 
                       << " " << L2_ek[k+1] / L2_ek[k] << std::endl;
    }
}

int main() {
    std::string square = "../meshes/square.msh";
    std::string square_output = "../result_square/Laplace/";
    int L = 5; // refinement steps
    
    std::vector<int> num_planwaves(L+1);
    num_planwaves[L] = 2;
    for(int i = L - 1; i >= 0; --i) {
        num_planwaves[i] = 2 * num_planwaves[i+1];
    }

    Harmonic_fun sol; 
    FHandle_t u = sol.get_fun();  // u(x,y) = exp(x+iy)
    FunGradient_t grad_u = sol.get_gradient();  
    Laplace laplace(L, square, u, num_planwaves);
    int nr_coarsemesh = 3;
    Laplace_mg(laplace, L, nr_coarsemesh, u);
}

