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

#include "../utils/utils.h"

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
class PUM_ElementMatrix{
public:
    using size_type = unsigned int;
    using Scalar = std::complex<double>;
    using Mat_t = Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic>;
    
    PUM_ElementMatrix(size_type N, double k, double alpha, double gamma, int degree=20): 
        N_(N), k_(k), alpha_(alpha), gamma_(gamma), degree_(degree) {}
    
    bool isActive(const lf::mesh::Entity & /*cell*/) { return true; }
    
    /*
     * @brief main routine for the computation of element matrices
     *
     * @param cell reference to the triangular cell for
     *        which the element matrix should be computed.
     * @return a square matrix with 3*N rows.
     */
    Mat_t Eval(const lf::mesh::Entity &cell){
            const lf::base::RefEl ref_el{cell.RefEl()};
        LF_ASSERT_MSG(ref_el == lf::base::RefEl::kTria(),
                    "Cell must be of triangle type");
        Mat_t elem_mat(3 * N_, 3 * N_);

        // Obtain the vertex coordinates of the cell, which completely
        // describe its shape.
        const lf::geometry::Geometry *geo_ptr = cell.Geometry();
        
        // Matrix storing corner coordinates in its columns(2x3 in this case)
        auto vertices = geo_ptr->Global(ref_el.NodeCoords());
        
        // suppose that the barycentric coordinate functions have the form
        // \lambda_i = a + b1*x+b2*y
        // \lambda_i = X(0,i) + X(1,i)*x + X(2,i)*y
        // grad \lambda_i = [X(1,i), X(2,i)]^T
        // grad \lambda_1_2_3 = X.block<2,3>(1,0)
        Eigen::Matrix3d X, tmp;
        tmp.block<3,1>(0,0) = Eigen::Vector3d::Ones();
        tmp.block<3,2>(0,1) = vertices.transpose();
        X = tmp.inverse();
        
        for(int i = 0; i < 3*N_; ++i){
            // ci = b_i1 * e_t1
            int i1 = i / N_;
            int t1 = i % N_;
            for(int j = 0; j < 3*N_; ++j) {
                // cj = b_j2 * e_t2
                // elem_mat(i,j) = aK(cj, ci)
                int j2 = j / N_;
                int t2 = j % N_;
                auto f = [this,&X,&i1,&j2,&t1,&t2](const Eigen::Vector2d& x)->Scalar {
                    Eigen::Vector2d di, dj, betai, betaj; 
                    double pi = std::acos(-1);
                    di << std::cos(2*pi*t1/N_), std::sin(2*pi*t1/N_);
                    dj << std::cos(2*pi*t2/N_), std::sin(2*pi*t2/N_);
                    betai << X(1, i1), X(2, i1);
                    betaj << X(1, j2), X(2, j2);
                    double lambdai = X(0,i1) + betai.dot(x);
                    double lambdaj = X(0,j2) + betaj.dot(x);

                    auto gradci = std::exp(1i*k_*di.dot(x)) * (betai + 1i*k_*lambdai*di);
                    auto gradcj = std::exp(1i*k_*dj.dot(x)) * (betaj + 1i*k_*lambdaj*dj);
                    auto val_ci = lambdai * std::exp(1i*k_*di.dot(x));
                    auto val_cj = lambdaj * std::exp(1i*k_*dj.dot(x));

                    // !!!! in Eigen, u.dot(v) return the hermitian dot product (equals to u.adjoint()*v)
                    return alpha_ * gradci.dot(gradcj) + gamma_ * val_cj * std::conj(val_ci);
                }; 
                elem_mat(i, j) = LocalIntegral(cell, degree_, f);
            }
        }
        return elem_mat;
    }
private:
    size_type N_; // number of planner waves
    double k_;
    double alpha_;
    double gamma_;
    int degree_;
};

