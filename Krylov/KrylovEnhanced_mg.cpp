#include <cmath>
#include <functional>
#include <fstream>
#include <iostream>
#include <string>

#include <lf/assemble/assemble.h>
#include <lf/base/base.h>
#include <lf/mesh/test_utils/test_meshes.h>
#include <lf/mesh/utils/utils.h>
#include <lf/refinement/refinement.h>
#include <lf/uscalfe/uscalfe.h>

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include "GMRES.h"
#include "../utils/HE_solution.h"
#include "../utils/utils.h"
#include "../LagrangeO1/HE_LagrangeO1.h"

using namespace std::complex_literals;

void O1_gmres_vcycle(Vec_t& u, Vec_t& f, std::vector<SpMat_t>& Op, std::vector<SpMat_t>& I, 
    double k, std::vector<double> mesh_width, size_type nu1, size_type nu2) {

    int L = I.size();
    std::vector<int> op_size(L+1);
    for(int i = 0; i <= L; ++i) {
        op_size[i] = Op[i].rows();
    }
    std::vector<Vec_t> initial(L + 1), rhs_vec(L + 1);

    initial[L] = u;
    rhs_vec[L] = f;
    // initial guess on coarser mesh are all zero
    for(int i = 0; i < L; ++i) {
        initial[i] = Vec_t::Zero(op_size[i]);
    }
    for(int i = L; i > 0; --i) {
        if(k * mesh_width[i] < 1.5 || k * mesh_width[i] > 8.0){
            Gaussian_Seidel(Op[i], rhs_vec[i], initial[i], 1, nu1);
        } else {
            gmres(Op[i], rhs_vec[i], initial[i], 10, 1);
        }
        rhs_vec[i-1] = I[i-1].transpose() * (rhs_vec[i] - Op[i] * initial[i]);
    }

    Eigen::SparseLU<SpMat_t> solver;
    solver.compute(Op[0]);
    initial[0] = solver.solve(rhs_vec[0]);
   
    for(int i = 1; i <= L; ++i) {
        initial[i] += I[i-1] * initial[i-1];
        if(k * mesh_width[i] < 1.5 || k * mesh_width[i] > 8.0){
            Gaussian_Seidel(Op[i], rhs_vec[i], initial[i], 1, nu2);
        } else {
            gmres(Op[i], rhs_vec[i], initial[i], 20, 1);
        }
    }
    u = initial[L];
}

Vec_t O1_gmres_vcycle(Vec_t f, std::vector<SpMat_t>& Op, std::vector<SpMat_t>& I, 
    double k, std::vector<double> mesh_width, size_type nu1, size_type nu2,
    double error_threshold) {

    int L = I.size();
    std::vector<int> op_size(L+1);
    for(int i = 0; i <= L; ++i) {
        op_size[i] = Op[i].rows();
    }
    std::vector<Vec_t> initial(L + 1), rhs_vec(L + 1);

    rhs_vec[L] = f;
    // initial guess on coarser mesh are all zero
    for(int i = 0; i <= L; ++i) {
        initial[i] = Vec_t::Zero(op_size[i]);
    }
    for(int i = L; i > 0; --i) {
        if(k * mesh_width[i] < 0.5 || k * mesh_width[i] > 8.0){
            Gaussian_Seidel(Op[i], rhs_vec[i], initial[i], 1, nu1);
        } else {
            gmres(Op[i], rhs_vec[i], initial[i], 20, error_threshold);
        }
        rhs_vec[i-1] = I[i-1].transpose() * (rhs_vec[i] - Op[i] * initial[i]);
    }

    Eigen::SparseLU<SpMat_t> solver;
    solver.compute(Op[0]);
    initial[0] = solver.solve(rhs_vec[0]);
   
    for(int i = 1; i <= L; ++i) {
        initial[i] += I[i-1] * initial[i-1];
        if(k * mesh_width[i] < 1.5 || k * mesh_width[i] > 8.0){
            Gaussian_Seidel(Op[i], rhs_vec[i], initial[i], 1, nu2);
        } else {
            gmres(Op[i], rhs_vec[i], initial[i], 20, error_threshold);
        }
    }
    return initial[L];
}

void mg_O1_gmres(HE_LagrangeO1& he_O1, int L, int nr_coarsemesh, double k, FHandle_t u) {
    auto eq_pair = he_O1.build_equation(L);
    SpMat_t A(eq_pair.first.makeSparse());

    std::vector<SpMat_t> Op(nr_coarsemesh + 1), prolongation_op(nr_coarsemesh);
    std::vector<double> ms(nr_coarsemesh + 1);
    auto mesh_width = he_O1.mesh_width();
    Op[nr_coarsemesh] = A;
    ms[nr_coarsemesh] = mesh_width[L];
    for(int i = nr_coarsemesh - 1; i >= 0; --i) {
        int idx = L + i - nr_coarsemesh;
        prolongation_op[i] = he_O1.prolongation(idx);
        Op[i] = prolongation_op[i].transpose() * Op[i+1] * prolongation_op[i];
        ms[i] = mesh_width[idx];
    }

    /**************************************************************************/
    auto zero_fun = [](const coordinate_t& x) -> Scalar { return 0.0; };

    int N = A.rows();
    Vec_t v = Vec_t::Random(N); // initial value
    Vec_t uh = he_O1.solve(L); // finite element solution

    int nu1 = 1, nu2 = 1;

    std::vector<double> L2_vk;
    std::vector<double> L2_ek;

    // std::cout << std::scientific << std::setprecision(1);
    for(int i = 0; i < 10; ++i) {
        std::cout << std::setw(15) << he_O1.L2_Err(L, v - uh, zero_fun) << " ";
        std::cout << std::setw(15) << he_O1.L2_Err(L, v, u) << std::endl;
        // std::cout << v.norm() << " " << std::endl;
        L2_vk.push_back(he_O1.L2_Err(L, v, zero_fun));
        L2_ek.push_back(he_O1.L2_Err(L, v - uh, zero_fun));
        O1_gmres_vcycle(v, eq_pair.second, Op, prolongation_op, k, ms, nu1, nu2);
    }

    std::cout << "||u-uh||_2 = " << he_O1.L2_Err(L, uh, u) << std::endl;
    std::cout << "||v_{i+1}||/||v_i||" << std::endl;
    std::cout << std::left;
    for(int i = 0; i + 1 < L2_vk.size(); ++i) {
        std::cout << i << " " << std::setw(10) << L2_vk[i+1] / L2_vk[i] 
                       << " " << std::setw(10) << L2_ek[i+1] / L2_ek[i] << std::endl;
    }
} 

/*
 * Use GMRES as smooting for intermidate coarse mesh
 * and use multigrid as preconditioner for outer GMRES iteration
 * suppose M is the preconditioner matrix, then MatVec product of inverse of M can
 * be obtained by running the multigrid.
 * 
 * Linear system in finest mesh: Ax = b
 * preconditioned system: AM^-1 z = b, Mx = z
 */
void KrylovEnhance(HE_LagrangeO1& he_O1, int L, int nr_coarsemesh, double k, FHandle_t u) {
    auto eq_pair = he_O1.build_equation(L);
    SpMat_t A(eq_pair.first.makeSparse());

    std::vector<SpMat_t> Op(nr_coarsemesh + 1), prolongation_op(nr_coarsemesh);
    std::vector<double> ms(nr_coarsemesh + 1);
    auto mesh_width = he_O1.mesh_width();
    Op[nr_coarsemesh] = A;
    ms[nr_coarsemesh] = mesh_width[L];
    for(int i = nr_coarsemesh - 1; i >= 0; --i) {
        int idx = L + i - nr_coarsemesh;
        prolongation_op[i] = he_O1.prolongation(idx);
        Op[i] = prolongation_op[i].transpose() * Op[i+1] * prolongation_op[i];
        ms[i] = mesh_width[idx];
    }
    Vec_t uh = he_O1.solve(L); // finite element solution
    auto zero_fun = [](const coordinate_t& x) -> Scalar { return 0.0; };
    std::vector<double> L2_vk;
    std::vector<double> L2_ek;

    /*
     * Outer iteration of GMRES, use MG as right preconditioner
     * since GMRES is used as smoothing in certain level of MG and 
     * GMRES is not linear iterations, so we need to use flexible GMRES here.
     */
    int n = A.rows();
    int m = 100;
    Vec_t x = Vec_t::Random(n); // initial value
    Vec_t x_copy = x;
    Vec_t b = eq_pair.second;
    Vec_t tmp = Vec_t::Random(n);

    Vec_t r = b - A * x;
    double b_norm = b.norm();
    double r_norm = r.norm();
    double error = r_norm / r_norm;

    // For givens roatation
    Vec_t sn = Vec_t::Zero(m), cs = Vec_t::Zero(m);
    Vec_t e1 = Vec_t::Zero(m+1);
    e1(0) = 1.0;
    std::vector<double> e;
    e.push_back(error);

    Vec_t beta = r_norm * e1;

    Mat_t V(n, m+1);
    V.col(0) = r / r_norm;
    Mat_t Z(n, m);
    Mat_t H = Mat_t::Zero(m+1, m);

    int j;
    for(j = 1; j <= m; ++j) {
        std::cout << std::setw(15) << he_O1.L2_Err(L, x - uh, zero_fun) << " ";
        std::cout << std::setw(15) << he_O1.L2_Err(L, x, u) << std::endl;
        // std::cout << v.norm() << " " << std::endl;
        L2_vk.push_back(he_O1.L2_Err(L, x, zero_fun));
        L2_ek.push_back(he_O1.L2_Err(L, x - uh, zero_fun));

        Z.col(j-1) = O1_gmres_vcycle(V.col(j-1), Op, prolongation_op, k, ms, 
            2, 2, 0.000001);
        Vec_t wj = A * Z.col(j-1);
        for(int i = 0; i < j; ++i) {
            H(i, j-1) = wj.dot(V.col(i));
            wj = wj - H(i, j-1) * V.col(i);
        }
        H(j, j-1) = wj.norm();
        V.col(j) = wj / H(j, j-1);
        
        // eliminate the last element in H jth row and update the rotation matrix
        applay_givens_roataion(H, cs, sn, j-1);
    
        // update the residual vector
        beta(j)  = -sn(j-1) * beta(j-1);
        beta(j-1) = cs(j-1) * beta(j-1);
        
        error = std::abs(beta(j)) / r_norm;
        e.push_back(error);

        // calculate the result
        Vec_t y = solve_upper_triangle(H.topLeftCorner(j, j), beta.head(j));
        x = x_copy + Z.leftCols(j) * y;
    }

    std::cout << "||u-uh||_2 = " << he_O1.L2_Err(L, uh, u) << std::endl;
    std::cout << "||v_{i+1}||/||v_i|| ||e_{i+1}||/||e_i||" << std::endl;
    std::cout << std::left;
    for(int i = 0; i + 1 < L2_vk.size(); ++i) {
        std::cout << i << " " << std::setw(10) << L2_vk[i+1] / L2_vk[i] 
                       << " " << std::setw(10) << L2_ek[i+1] / L2_ek[i] << std::endl;
    }
}

int KrylovEnhance(HE_LagrangeO1& he_O1, int L, int nr_coarsemesh, double k, 
    double error_threshold, bool verbose) {
    auto eq_pair = he_O1.build_equation(L);
    SpMat_t A(eq_pair.first.makeSparse());

    std::vector<SpMat_t> Op(nr_coarsemesh + 1), prolongation_op(nr_coarsemesh);
    std::vector<double> ms(nr_coarsemesh + 1);
    auto mesh_width = he_O1.mesh_width();
    Op[nr_coarsemesh] = A;
    ms[nr_coarsemesh] = mesh_width[L];
    for(int i = nr_coarsemesh - 1; i >= 0; --i) {
        int idx = L + i - nr_coarsemesh;
        prolongation_op[i] = he_O1.prolongation(idx);
        Op[i] = prolongation_op[i].transpose() * Op[i+1] * prolongation_op[i];
        ms[i] = mesh_width[idx];
    }
    Vec_t uh = he_O1.solve(L); // finite element solution
   
    /*
     * Outer iteration of GMRES, use MG as right preconditioner
     * since GMRES is used as smoothing in certain level of MG and 
     * GMRES is not linear iterations, so we need to use flexible GMRES here.
     */
    int n = A.rows();
    int m = 200;
    Vec_t x = Vec_t::Random(n); // initial value
    Vec_t b = eq_pair.second;
    Vec_t tmp = Vec_t::Random(n);
     
    Vec_t r = b - A * x;
    double r_norm = r.norm();
    double error = r_norm / r_norm;

    // For givens roatation
    Vec_t sn = Vec_t::Zero(m), cs = Vec_t::Zero(m);
    Vec_t e1 = Vec_t::Zero(m+1);
    e1(0) = 1.0;
    std::vector<double> e;
    e.push_back(error);

    Vec_t beta = r_norm * e1;

    Mat_t V(n, m+1);
    V.col(0) = r / r_norm;
    Mat_t Z(n, m);
    Mat_t H = Mat_t::Zero(m+1, m);

    int j;
    if(verbose) {
        std::cout << "m  ||rm||" << std::endl;
    }
    for(j = 1; j <= m; ++j) {
        Z.col(j-1) = O1_gmres_vcycle(V.col(j-1), Op, prolongation_op, k, ms, 
            2, 2, error_threshold);
        Vec_t wj = A * Z.col(j-1);
        for(int i = 0; i < j; ++i) {
            H(i, j-1) = wj.dot(V.col(i));
            wj = wj - H(i, j-1) * V.col(i);
        }
        H(j, j-1) = wj.norm();
        V.col(j) = wj / H(j, j-1);
        
        // eliminate the last element in H jth row and update the rotation matrix
        applay_givens_roataion(H, cs, sn, j-1);
    
        // update the residual vector
        beta(j)  = -sn(j-1) * beta(j-1);
        beta(j-1) = cs(j-1) * beta(j-1);
        
        error = std::abs(beta(j)) / r_norm;
        e.push_back(error);
        if(verbose) std::cout << j << "  " << error << std::endl;
        if(error < error_threshold) {
            break; 
        }
    }
    // calculate the result
    if(j == m+1) --j;
    Vec_t y = solve_upper_triangle(H.topLeftCorner(j, j), beta.head(j));
    x = x + Z.leftCols(j) * y;
    return j;
}

void iteration_count() {
    std::string output = "../result/mgGmres_count/";
    std::vector<std::string> meshes{"../meshes/square.msh", "../meshes/square_hole2.msh", "../meshes/triangle_hole.msh"};
    // std::vector<std::string> meshes{"../meshes/square_hole2.msh"};
    std::vector<bool> hole{false, true, true};
    // std::vector<int> wave_number{4, 6, 8, 10, 12, 14, 16, 18, 20};
    std::vector<int> wave_number{8, 10, 12};
    int L = 4;
    int num_coarserlayer = L;

    double error_threshold = 0.000001;

    int nr_mesh = meshes.size();
    std::vector<std::vector<int>> counts(nr_mesh+1);
    counts[nr_mesh] = wave_number;
    for(int mesh_idx = 0; mesh_idx < nr_mesh; ++mesh_idx) {
        for(int k_idx = 0; k_idx < wave_number.size(); ++k_idx) {
            double k = wave_number[k_idx];
            plan_wave sol(k, 0.8, 0.6);
            auto u = sol.get_fun();
            auto g = sol.boundary_g();

            HE_LagrangeO1 he_O1(L, k, meshes[mesh_idx], g, u, hole[mesh_idx], 50);
            int it_count = KrylovEnhance(he_O1, L, num_coarserlayer, k, error_threshold, false);
            counts[mesh_idx].push_back(it_count);
        }
        std::cout << meshes[mesh_idx] << " finished" << std::endl;
    } 
    std::vector<std::string> mesh_name{"unit_square", "square_hole", "triangle_hole", "k"};
    // std::vector<std::string> mesh_name{"square_hole", "k"};
    std::string str = "L" + std::to_string(L);
    print_save_error(counts, mesh_name, str, output);
}

int main(){
    std::string square = "../meshes/square.msh";
    std::string square_hole2 = "../meshes/square_hole2.msh";
    std::string triangle_hole = "../meshes/triangle_hole.msh";

    size_type L = 4; // refinement steps
    double k = 20.0; // wave number
    plan_wave sol(k, 0.8, 0.6);
    auto u = sol.get_fun();
    auto g = sol.boundary_g();
    auto grad_u = sol.get_gradient();
    // HE_LagrangeO1 he_O1(L, k, square, g, u, false, 50);
    // HE_LagrangeO1 he_O1(L, k, square_hole2, g, u, true, 50);
    HE_LagrangeO1 he_O1(L, k, triangle_hole, g, u, true, 50);
    
    int num_coarserlayer = L;
    // mg_O1_gmres(he_O1, L, num_coarserlayer, k, u);
    // KrylovEnhance(he_O1, L, num_coarserlayer, k, u);
    iteration_count();
}
