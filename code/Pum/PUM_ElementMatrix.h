#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

#include <lf/assemble/assemble.h>
#include <lf/base/base.h>
#include <lf/io/io.h>
#include <lf/mesh/test_utils/test_meshes.h>
#include <lf/mesh/utils/utils.h>
#include <lf/refinement/refinement.h>
#include <lf/uscalfe/uscalfe.h>

#include <Eigen/Core>
#include <Eigen/SparseCore>

using namespace std::complex_literals;

/* 
 * This class is for local quadrature based computations for PUM spaces.
 * PUM spaces: {{bi(x) * exp(ikdt x}}
 * 
 * The element matrix is corresponding to the (local) bilinear form
 * (u, v) -> \int_K alpha*(grad u, grad v) + gamma * (u,v)dx
 * 
 * Member N is the number of waves
 * plan wave: e_t = exp(i*k*(dt1 * x(0) + dt2 * x(1))),
 * frequency: dt1 = cos(t/N * 2pi), dt2 = sin(t/N * 2pi)
 */
class PUM_FEElementMatrix{
public:
    using size_type = unsigned int;
    using Scalar = std::complex<double>;
    using Mat_t = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    
    PUM_FEElementMatrix(size_type N_, double k_, double alpha_, double gamma_): 
        N(N_), k(k_), alpha(alpha_), gamma(gamma_){}
    
    bool isActive(const lf::mesh::Entity & /*cell*/) { return true; }
    
    /*
     * @brief main routine for the computation of element matrices
     *
     * @param cell reference to the triangular cell for
     *        which the element matrix should be computed.
     * @return a square matrix with 3*N rows.
     */
    Mat_t Eval(const lf::mesh::Entity &cell);
private:
    size_type N; // number of planner waves
    double k;
    double alpha;
    double gamma;
};
