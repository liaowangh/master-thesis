#include <cmath>
#include <vector>

#include "PUM_ElementMatrix.h"
#include "../utils/utils.h"
#include "../utils/triangle_integration.h"

using namespace std::complex_literals;

// a(u,v) = alpha * (grad u, grad v) + gamma * (u, v)
PUM_FEElementMatrix::Mat_t PUM_FEElementMatrix::Eval(const lf::mesh::Entity& cell) {
    const lf::base::RefEl ref_el{cell.RefEl()};
    LF_ASSERT_MSG(ref_el == lf::base::RefEl::kTria(),
                  "Cell must be of triangle type");
    Mat_t elem_mat(3 * N, 3 * N);

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
    
    for(int i = 0; i < 3*N; ++i){
        // ci = b_i1 * e_t1
        int i1 = i / N;
        int t1 = i % N;
        for(int j = 0; j < 3*N; ++j) {
            // cj = b_j2 * e_t2
            // elem_mat(i,j) = aK(cj, ci)
            int j2 = j / N;
            int t2 = j % N;
            auto f = [this,&X,&i1,&j2,&t1,&t2](const Eigen::Vector2d& x)->Scalar {
                Eigen::Vector2d di, dj, betai, betaj; 
                double pi = std::acos(-1);
                di << std::cos(2*pi*t1/N), std::sin(2*pi*t1/N);
                dj << std::cos(2*pi*t2/N), std::sin(2*pi*t2/N);
                betai << X(1, i1), X(2, i1);
                betaj << X(1, j2), X(2, j2);
                double lambdai = X(0,i1) + betai.dot(x);
                double lambdaj = X(0,j2) + betaj.dot(x);

                auto gradci = std::exp(1i*k*di.dot(x)) * (betai + 1i*k*lambdai*di);
                auto gradcj = std::exp(1i*k*dj.dot(x)) * (betaj + 1i*k*lambdaj*dj);
                auto val_ci = lambdai * std::exp(1i*k*di.dot(x));
                auto val_cj = lambdaj * std::exp(1i*k*dj.dot(x));

                // !!!! in Eigen, u.dot(v) return the hermitian dot product (equals to u.adjoint()*v)
                return alpha * gradci.dot(gradcj) + gamma * val_cj * std::conj(val_ci);
                // return alpha * gradcj.dot(gradci.conjugate()) + gamma * val_cj * std::conj(val_ci);
            }; 
            elem_mat(i, j) = LocalIntegral(cell, 10, f);
        }
    }
    return elem_mat;
}

// analytic formulation
/*
std::vector<Scalar> coefficient_comp(std::vector<Scalar> a, std::vector<Scalar> b) {
    // (a[0]*x+a[1]*y+a[2]) * (b[0]*x+b[1]*y+b[2])
    // = a[0]*b[0]*x^2 + a[1]*b[1]*y^2 + a[0]*b[1]+a[1]*b[0]*xy
    // + (a[0]*b[2]+a[2]*b[0])*x + a[1]*b[2]+a[2]*b[1]*y + a[2]*b[2]
    return {a[0]*b[0], a[1]*b[1], a[0]*b[1]+a[1]*b[0],
            a[0]*b[2]+a[2]*b[0], a[1]*b[2]+a[2]*b[1], a[2]*b[2]};
}

PUM_FEElementMatrix::Mat_t PUM_FEElementMatrix::Eval(const lf::mesh::Entity &cell) {
    // cell contribution only, for l < L. for l == L, just the reaction diffusion element matrix provider
    
    size_type N = (1 << (L + 1 - l));
    elem_mat_t elem_mat(3*N, 3*N);
    
    // Topological type of the cell
    const lf::base::RefEl ref_el{cell.RefEl()};
    
    // Obtain the vertex coordinates of the cell, which completely
    // describe its shape.
    const lf::geometry::Geometry *geo_ptr = cell.Geometry();
    
    // Matrix storing corner coordinates in its columns(2x3 in this case)
    auto vertices = geo_ptr->Global(ref_el.NodeCoords());
    
    // suppose that the barycentric coordinate functions have the form
    // a + b1*x+b2*y
    // \lambda_i = X(0,i) + X(1,i)*x + X(2,i)*y
    // grad \lambda_i = [X(1,i), X(2,i)]^T
    // grad \lambda_1_2_3 = X.block<2,3>(1,0)
    Eigen::Matrix3d X;
    X.block<3,1>(0,0) = Eigen::Vector3d::Ones();
    X.block<3,2>(0,1) = vertices.transpose();
    X = X.inverse();
    
    // \Phi = F_k*x+\tau: affine transform from unit triangle to the cell
    Eigen::Matrix2d Fk;
    Fk << vertices(0,1) - vertices(0,0), vertices(0,2) - vertices(0,0),
          vertices(1,1) - vertices(1,0), vertices(1,2) - vertices(1,0);
    Eigen::Vector2d tau << vertices(0,0), vertices(1,0);
    
    Eigen::Vector2d d1, d2, beta1, beta2;
    std::vector<std::vector<double>> lambda_coeff{
        {-1, -1, 1}, // -x-y+1
        {1, 0, 0},   // x
        {0, 1, 0}    // y
    };
    
    // compute a(h_i1^j1, h_i2^j2), stores in elem_mat[(i1-1)*N+j1, (i2-1)*N+j2]
    // the result if \int_K [-beta1.dot(beta2)-ik*beta2.dot(d1)*\lambda_i1-ik*beta1.dot(d2)*\lambda_i2
    //  +k^2*(1+d1.dot(d2)\lambda_i1*\lambda_i2]*exp(ik(d1+d2).dot(x))dx
    for(int i1 = 1; i1 <=3; i1++) {
        beta1 = X.block<2, 1>(1, i1-1);
        for(int i2 = 1; i2 <= 3; i2++) {
            beta2 = X.block<2, 1>(1, i2-1);
            std::vector<double> coeff = coefficient_comp(lambda_coeff[i1-1], lambda_coeff[i2-1]);
            for(int j1 = 0; j1 < N; j1++) {
                d1 << std::cos(2*M_PI*j1 / N), std::sin(2*M_PI*j1 / N);
                for(int j2 = 0; j2 < N; j2++) {
                    d2 << std::cos(2*M_PI*j2 / N), std::sin(2*M_PI*j2 / N);
                    
                    vector<vector<mat_scalar>> coeff_tmp(4, vector<mat_scalar>(6));
                    coeff_tmp[0] = {0, 0, 0, 0, 0, -beta1.dot(beta2)}; // -beta1.dot(beta2)
                    coeff_tmp[1] = coefficient_comp(lambda_coeff[i1-1],
                                                    {0, 0, -1i * beta2.dot(d1)}); // -ik*beta2.dot(d1)*\lambda_i1
                    coeff_tmp[2] = coefficient_comp(lambda_coeff[i2-1],
                                                    {0, 0, -1i * beta1.dot(d2)}); // -ik*beta1.dot(d2)*\lambda_i2
                    coeff_tmp[3] = coefficient_comp(lambda_coeff[i1-1], lambda_coeff[i2-1]); // \lambda_i1 * \lambda_i2
                    
                    for(int i = 0; i < coeff_tmp[3].size(); ++i){
                        coeff_tmp[3][i] *= k*k*(1+d1.dot(d2));
                    }
                    
                    vector<mat_scalar> coeff(6, 0);
                    for(int i = 0; i < 6; ++i){
                        for(int j = 0; j < 4; ++j){
                            coeff[i][j] += coeff_tmp[j][i];
                        }
                    }
                    
                    elmat((i1-1)*N+j1, (i2-1)*N+j2) = std::abs(Fk.determinant()) * std::exp(1i*k*(d1+d2).dot(tau)) * int_fxy_exp(k, coeff, Fk.transpose()*(d1 + d2));
                }
            }
        }
    }
    return elmat;
}
*/