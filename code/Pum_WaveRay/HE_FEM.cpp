#include "HE_FEM.h"

std::vector<double> HE_FEM::mesh_width() {
    std::vector<double> width(L_+1); // L_+1 meshes in total
    auto mesh = getmesh(0); // coarest mesh
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

lf::mesh::utils::CodimMeshDataSet<bool> 
HE_FEM::outerBdy_selector(size_type l) {
    auto mesh = getmesh(l);
    auto outer_boundary{lf::mesh::utils::flagEntitiesOnBoundary(mesh, 1)};
    if(hole_exist_) {
        auto outer_nr = reader_->PhysicalEntityName2Nr("outer_boundary");
        auto inner_nr = reader_->PhysicalEntityName2Nr("inner_boundary");

        // modify it to classify inner and outer boundary
        for(const lf::mesh::Entity* edge: mesh->Entities(1)) {
            if(outer_boundary(*edge)) {
                // find a boundary edge, need to determine if it's outer boundary
                const lf::mesh::Entity* parent_edge = edge;
                for(int i = l; i > 0; --i) {
                    parent_edge = mesh_hierarchy_->ParentEntity(i, *parent_edge);
                }
                if(reader_->IsPhysicalEntity(*parent_edge, inner_nr)) {
                    // it is the inner boundary
                    outer_boundary(*edge) = false;
                }
            }
        }
    }
    return outer_boundary;
}

lf::mesh::utils::CodimMeshDataSet<bool> 
HE_FEM::innerBdy_selector(size_type l) {
    auto mesh = getmesh(l);
    lf::mesh::utils::CodimMeshDataSet<bool> inner_boundary(mesh, 1, false);
    if(hole_exist_) {
        auto inner_nr = reader_->PhysicalEntityName2Nr("inner_boundary");
        // modify it to classify inner and outer boundary
        for(const lf::mesh::Entity* edge: mesh->Entities(1)) {
            const lf::mesh::Entity* parent_edge = edge;
            for(int i = l; i > 0; --i) {
                parent_edge = mesh_hierarchy_->ParentEntity(i, *parent_edge);
            }
            if(reader_->IsPhysicalEntity(*parent_edge, inner_nr)) {
                // it is the inner boundary
                inner_boundary(*edge) = true;
            }
        }
    }
    return inner_boundary;
}

// S_l -> S_{l+1}， 0 <= l < L_
HE_FEM::SpMat_t HE_FEM::prolongation_lagrange(size_type l) {
    LF_ASSERT_MSG(l >= 0 && l < L_, "l in prolongation should be smaller to L_");
    auto coarse_mesh = getmesh(l);
    auto fine_mesh   = getmesh(l+1);
    
    auto coarse_dofh = lf::assemble::UniformFEDofHandler(coarse_mesh, 
                        {{lf::base::RefEl::kPoint(), 1}});
    auto fine_dof    = lf::assemble::UniformFEDofHandler(fine_mesh, 
                        {{lf::base::RefEl::kPoint(), 1}});

    size_type n_c = coarse_dofh.NumDofs();
    size_type n_f = fine_dof.NumDofs();
    
    Mat_t M = Mat_t::Zero(n_c, n_f);
    // SpMat_t M(n_f, n_c);
    // std::vector<triplet_t> triplets;

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
                // triplets.push_back(triplet_t(fine_mesh->Index(*points[j]), 
                //     coarse_mesh->Index(*parent_p), 1.0));
                // triplets.push_back(triplet_t(fine_mesh->Index(*points[1-j]),
                //     coarse_mesh->Index(*parent_p), 0.5));
            }
        }
    }
    return (M.transpose()).sparseView();
    // M.setFromTriplets(triplets.begin(), triplets.end());
    // return M;
}

/*
 * E_l -> E_{l+1}, l = 0, 1, ..., L_-1
 * P(e_2t_l) = e_t^{l+1}
 * P(e_{2t+1}_l) = at * e_t_{l+1} + bt * e_{t+1}_{l+1} (get the coefficient by L2 projection)
 * e_t^l = exp(ik(cos(2pi*t/N)x + sin(2pi*t/N)y)) t = 0, 1, ..., N-1 (note that in my thesis, t starts from 1)
 */
HE_FEM::SpMat_t HE_FEM::prolongation_planwave(size_type l) {
    auto mesh = getmesh(0);
    double domain_area = 0.0;
    for(const lf::mesh::Entity* cell: mesh->Entities(0)) {
        const lf::geometry::Geometry *geo_ptr = cell->Geometry();
        domain_area += lf::geometry::Volume(*geo_ptr);
    }

    mesh = getmesh(L_); // numerical quadrature is carried out in this mesh
    double pi = std::acos(-1);

    // for(int l = 0; l < L_; ++l) {
    int N1 = num_planewaves_[l];
    int N2 = num_planewaves_[l+1];

    // Mat_t M = Mat_t::Zero(N2, N1);
    SpMat_t M(N2, N1);
    std::vector<triplet_t> triplets;

    for(int t = 0; t < N2; ++t) {
        
        Eigen::Vector2d dl1_t1, dl1_t, dl_2t1;

        dl1_t1 << std::cos(2*pi*(t+1) / N2), std::sin(2*pi*(t+1) / N2); // e^{l+1}_{t+1}
        dl1_t  << std::cos(2*pi* t    / N2), std::sin(2*pi* t    / N2); // e^{l+1}_t
        dl_2t1 << std::cos(2*pi*(2*t+1)/N1), std::sin(2*pi*(2*t+1)/N1); // e^l_{2t+1}

        auto f1 = [&dl1_t1, &dl1_t, this](const Eigen::Vector2d& x)->Scalar {
            return std::exp(1i*k_*(dl1_t1 - dl1_t).dot(x));
        };

        lf::mesh::utils::MeshFunctionGlobal mf_f1{f1};
        
        Scalar A12 = lf::uscalfe::IntegrateMeshFunction(*mesh, mf_f1, 10);

        Eigen::Matrix<Scalar, 2, 2> A;
        A << domain_area, A12, std::conj(A12), domain_area;

        auto f2 = [&dl_2t1, &dl1_t, this](const Eigen::Vector2d& x)->Scalar {
            return std::exp(1i*k_*(dl_2t1 - dl1_t).dot(x));
        };

        auto f3 = [&dl_2t1, &dl1_t1, this](const Eigen::Vector2d& x)->Scalar {
            return std::exp(1i*k_*(dl_2t1 - dl1_t1).dot(x));
        };

        lf::mesh::utils::MeshFunctionGlobal mf_f2{f2};
        lf::mesh::utils::MeshFunctionGlobal mf_f3{f3};
        Eigen::Matrix<Scalar, 2, 1> b;
        b << lf::uscalfe::IntegrateMeshFunction(*mesh, mf_f2, 10),
                lf::uscalfe::IntegrateMeshFunction(*mesh, mf_f3, 10);
        auto tmp = A.colPivHouseholderQr().solve(b);

        // M(t, 2 * t) = 1;
        // M(t, 2 * t + 1) = tmp(0);
        // M((t+1) % N2, 2 * t + 1) = tmp(1);
        triplets.push_back(triplet_t(t, 2*t, 1.0));
        triplets.push_back(triplet_t(t, 2*t+1, tmp(0)));
        triplets.push_back(triplet_t((t+1)%N2, 2*t+1, tmp(1)));
        // triplets.push_back(triplet_t(t, 2*t+1, 0.5));
        // triplets.push_back(triplet_t((t+1)%N2, 2*t+1, 0.5));
    }
    // return M;
    M.setFromTriplets(triplets.begin(), triplets.end());
    return M;
}

/*
 * SxE_{L_-1} -> S_L
 */
HE_FEM::SpMat_t HE_FEM::prolongation_SE_S() {
    double pi = std::acos(-1.);
    auto Q = prolongation_lagrange(L_-1);
    int n = Q.rows();
    int N = num_planewaves_[L_-1];

    Mat_t E = Mat_t::Zero(n, N);

    auto S_mesh = getmesh(L_);
    auto S_dofh = lf::assemble::UniformFEDofHandler(S_mesh, 
                        {{lf::base::RefEl::kPoint(), 1}});
    for(int i = 0; i < n; ++i) {
        const lf::mesh::Entity& vi = S_dofh.Entity(i); // the entity to which i-th global shape function is associated
        coordinate_t vi_coordinate = lf::geometry::Corners(*vi.Geometry()).col(0);
        for(int t = 0; t < N; ++t) {
            Eigen::Vector2d dt;
            dt << std::cos(2*pi*t/N), std::sin(2*pi*t/N);
            E(i,t) = std::exp(1i*k_*dt.dot(vi_coordinate));
        }
    }

    // Mat_t res(n, Q.cols() * N);
    // for(int i = 0; i < n; ++i) {
    //     for(int j = 0; j < Q.cols(); ++j) {
    //         res.block(i, j*N, 1, N) = Q(i,j) * E.row(i);
    //     }
    // }
    // for(int j = 0; j < Q.cols(); ++j) {
    //     res.block(0, j*N, n, N) = Q.col(j).asDiagonal() * E;
    // }

    SpMat_t res(n, Q.cols() * N);
    std::vector<triplet_t> triplets;
    for(int outer_idx = 0; outer_idx < Q.outerSize(); ++outer_idx) {
        for(SpMat_t::InnerIterator it(Q, outer_idx); it; ++it) {
            int i = it.row();
            int j = it.col();
            Scalar qij = it.value(); 
            for(int t = 0; t < N; ++t) {
                triplets.push_back(triplet_t(i, j*N+t, qij*E(i,t)));
            }
        }
    }
    res.setFromTriplets(triplets.begin(), triplets.end());
    return res;
}

void HE_FEM::vector_vtk(size_type l, const Vec_t& v, const std::string& name_str) {
    auto mesh = getmesh(l);
    LF_ASSERT_MSG(v.size() == mesh->NumEntities(2), 
        "vector size should be same as number of vertices in mesh.");
    lf::io::VtkWriter vtk_writer(mesh, "../vtk_file/" + name_str + ".vtk");

    // write nodal data v
    // lf::mesh::utils::CodimMeshDataSet<double> data(mesh, 2); // codim 2
    lf::mesh::utils::CodimMeshDataSet<Eigen::Vector2d> data(mesh, 2);
    for(const auto* node: mesh->Entities(2)) {
        Scalar value = v(mesh->Index(*node));
        Eigen::Vector2d tmp;
        tmp << std::real(value), std::imag(value);
        data(*node) = tmp;
    }

    vtk_writer.WritePointData("mode", data);
}