#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include "../../utils/HE_solution.h"
#include "../../utils/utils.h"
#include "../PUM_WaveRay.h"

using namespace std::complex_literals;

using Scalar = std::complex<double>;
using size_type = unsigned int;

// void waveray_vcycle(Vec_t& u, Vec_t& f, std::vector<SpMat_t>& Op, std::vector<SpMat_t>& I, 
//     std::vector<int>& stride, double k, std::vector<double> mesh_width, 
//     size_type mu1, size_type mu2, bool solve_on_coarest) {
//     int L = I.size();
//     LF_ASSERT_MSG(Op.size() == L + 1 && stride.size() == L + 1, 
//         "#{transfer operator} should be #{Operator} - 1");
//     std::vector<int> op_size(L+1);
//     for(int i = 0; i <= L; ++i) {
//         op_size[i] = Op[i].rows();
//     }
//     for(int i = 0; i < L; ++i) {
//         LF_ASSERT_MSG(I[i].rows() == op_size[i+1] && I[i].cols() == op_size[i],
//             "transfer operator size does not mathch grid operator size.");
//     }
//     std::vector<Vec_t> initial(L + 1), rhs_vec(L + 1);
//     initial[L] = u;
//     rhs_vec[L] = f;
//     // initial guess on coarser mesh are all zero
//     for(int i = 0; i < L; ++i) {
//         initial[i] = Vec_t::Zero(op_size[i]);
//     }
//     for(int i = L; i > 0; --i) {
//         if(mesh_width[i] * k < 2.0 || mesh_width[i] * k > 6.0) {
//             Gaussian_Seidel(Op[i], rhs_vec[i], initial[i], stride[i], mu1);
//         } else {
//             // block_GS(Op[i], rhs_vec[i], initial[i], stride[i], mu1);
//             // Kaczmarz(Op[i], rhs_vec[i], initial[i], 5 * mu1);
//         }
//         rhs_vec[i-1] = I[i-1].transpose() * (rhs_vec[i] - Op[i] * initial[i]);
//     }
//     if(solve_on_coarest) {
//         Eigen::SparseLU<SpMat_t> solver;
//         solver.compute(Op[0]);
//         initial[0] = solver.solve(rhs_vec[0]);
//     } else {
//         if(mesh_width[0] * k < 2.0 || mesh_width[0] * k > 6.0) {
//             Gaussian_Seidel(Op[0], rhs_vec[0], initial[0], stride[0], mu1+mu2);
//         } else {
//             // Kaczmarz(Op[0], rhs_vec[0], initial[0], 5 * mu1);
//         }
//     }
//     for(int i = 1; i <= L; ++i) {
//         initial[i] += I[i-1] * initial[i-1];
//         if(mesh_width[i] * k < 2.0 || mesh_width[i] * k > 6.0) {
//             Gaussian_Seidel(Op[i], rhs_vec[i], initial[i], stride[i], mu1);
//         } else {
//             // block_GS(Op[i], rhs_vec[i], initial[i], stride[i], mu1);
//             // Kaczmarz(Op[i], rhs_vec[i], initial[i], 5*mu2);
//         }
//     }
//     u = initial[L];
// }

void waveray_mg(PUM_WaveRay& p_waveray, int L, int num_wavelayer, 
    double k, FHandle_t u, bool solve_coarest) {

    auto eq_pair = p_waveray.build_equation(L);
    SpMat_t A(eq_pair.first.makeSparse());

    std::vector<SpMat_t> Op(num_wavelayer + 1), prolongation_op(num_wavelayer);
    std::vector<int> stride(num_wavelayer + 1);
    std::vector<double> ms(num_wavelayer + 1);
    auto mesh_width = p_waveray.mesh_width();
    Op[num_wavelayer] = A;
    stride[num_wavelayer] = 1;
    ms[num_wavelayer] = mesh_width[L];
    for(int i = num_wavelayer - 1; i >= 0; --i) {
        int idx = L + i - num_wavelayer;
        prolongation_op[i] = p_waveray.prolongation(idx);
        Op[i] = prolongation_op[i].transpose() * Op[i+1] * prolongation_op[i];
        // stride[i] = p_waveray.num_planwaves[idx];
        stride[i] = 1;
        ms[i] = mesh_width[idx];
    }

    // for(int i = 0; i < Op.size(); ++i) {
    //     std::cout << "Power iteration of Mesh Operator " << i << std::endl;
    //     Mat_t dense_A = Mat_t(Op[i]);
    //     Mat_t A_L = Mat_t(dense_A.triangularView<Eigen::Lower>());
    //     Mat_t A_U = A_L - dense_A;
    //     Mat_t GS_op = A_L.colPivHouseholderQr().solve(A_U);
    //     Vec_t eivals = GS_op.eigenvalues();
    //
    //     Scalar domainant_eival = eivals(0);
    //     for(int i = 1; i < eivals.size(); ++i) {
    //         if(std::abs(eivals(i)) > std::abs(domainant_eival)) {
    //             domainant_eival = eivals(i);
    //         }
    //     }
    //     if(i == 0) std::cout << eivals << std::endl;
    //     // std::cout << eivals << std::endl;
    //     std::cout << "Domainant eigenvalue: " << domainant_eival << std::endl;
    //     std::cout << "Absolute value: " << std::abs(domainant_eival) << std::endl;
    // }

    /***********************************************************************/
    auto zero_fun = [](const coordinate_t& x) -> Scalar { return 0.0; };

    int N = A.rows();
    Vec_t v = Vec_t::Random(N); // initial value
    Vec_t uh = p_waveray.solve(L);  // finite element solution

    int nu1 = 3, nu2 = 3;

    std::vector<double> L2_vk;
    std::vector<double> L2_ek;

    // std::cout << std::scientific << std::setprecision(1);
    for(int k = 0; k < 10; ++k) {
        std::cout << p_waveray.L2_Err(L, v - uh, zero_fun) << " ";
        std::cout << p_waveray.L2_Err(L, v, u) << std::endl;
        L2_vk.push_back(p_waveray.L2_Err(L, v, zero_fun));
        L2_ek.push_back(p_waveray.L2_Err(L, v - uh, zero_fun));
        // p_waveray.HE_LagrangeO1::solve_multigrid(v, L, 3, 3, 3, true);
        // waveray_vcycle(v, eq_pair.second, Op, prolongation_op, stride, k, ms, nu1, nu2, solve_coarest);
        v_cycle(v, eq_pair.second, Op, prolongation_op, stride, nu1, nu2, solve_coarest);
        // HE_LagrangeO1::solve_multigrid(v, L, 3, 3, 3, false);
    }
    std::cout << "||u-uh||_2 = " << p_waveray.L2_Err(L, uh, u) << std::endl;
    std::cout << "||v_{k+1}||/||v_k||" << std::endl;
    for(int k = 0; k + 1 < L2_vk.size(); ++k) {
        std::cout << k << " " << L2_vk[k+1] / L2_vk[k] 
                       << " " << L2_ek[k+1] / L2_ek[k] << std::endl;
    }
} 

/*
 * For square_hole.msh, h = 2^{-L-1}, and we can choose k = 2^{L-1}*pi
 * For square.msh, h_L = =2^{-L}, and we choose k = 2^{L-1}, then h_L * k = 0.5
 */
int main(){
    std::string square_output = "../result_square/ExtendPUM_WaveRay/";
    std::string square_hole_output = "../result_squarehole/ExtendPUM_WaveRaay/";
    std::string square = "../meshes/square.msh";
    std::string square_hole = "../meshes/square_hole.msh";
    size_type L = 5; // refinement steps
 
    double k = 2.0; // wave number
    std::vector<int> num_planwaves(L+1);
    num_planwaves[L] = 2;
    for(int i = L - 1; i >= 0; --i) {
        num_planwaves[i] = 2 * num_planwaves[i+1];
    }

    auto zero_fun = [](const coordinate_t& x)->Scalar { return 0.0; };
    plan_wave sol(k, 0.8, 0.6);
    auto u = sol.get_fun();
    auto grad_u = sol.get_gradient();
    auto g = sol.boundary_g();
    PUM_WaveRay p_waveray(L, k, square, g, u, false, num_planwaves, 50);

    int num_wavelayer = 1;
    waveray_mg(p_waveray, L, num_wavelayer, k, u, true);
}