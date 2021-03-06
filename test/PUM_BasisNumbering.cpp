#include <cmath>
#include <string>

#include <boost/filesystem.hpp>

#include <lf/assemble/assemble.h>
#include <lf/base/base.h>
#include <lf/mesh/test_utils/test_meshes.h>
#include <lf/mesh/utils/utils.h>
#include <lf/uscalfe/uscalfe.h>

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include "../utils/HE_solution.h"
#include "../utils/utils.h"

using namespace std::complex_literals;

int main(){
    // mesh path
    boost::filesystem::path here = __FILE__;
    // auto mesh_path = (here.parent_path().parent_path() / ("meshes/square.msh")).string(); 
    auto mesh_path = (here.parent_path().parent_path() / ("meshes/tri2.msh")).string();   
    auto mesh_factory = std::make_unique<lf::mesh::hybrid2d::MeshFactory>(2);
    lf::io::GmshReader reader(std::move(mesh_factory), mesh_path);
    int L = 2;
    std::shared_ptr<lf::refinement::MeshHierarchy> mesh_hierarchy = 
        lf::refinement::GenerateMeshHierarchyByUniformRefinemnt(reader.mesh(), L);
    
    // int level = 1;
    
    // auto mesh = mesh_hierarchy->getMesh(level);

    // int num_wave = 2;

    // lf::assemble::UniformFEDofHandler dofh(mesh, {{lf::base::RefEl::kPoint(), num_wave}});

    // // number of degrees of freedom managed by the DofhHandler object
    // const lf::assemble::size_type N_dofs(dofh.NumDofs());

    // auto outer_boundary{lf::mesh::utils::flagEntitiesOnBoundary(mesh, 1)};

    // auto outer_nr = reader.PhysicalEntityName2Nr("outer_boundary");
    // auto inner_nr = reader.PhysicalEntityName2Nr("inner_boundary");

    // // modify it to classify inner and outer boundary
    // for(const lf::mesh::Entity* edge: mesh->Entities(1)) {
    //     if(outer_boundary(*edge)) {
    //         // find a boundary edge, need to determine if it's outer boundary
    //         const lf::mesh::Entity* parent_edge = edge;
    //         for(int i = level; i > 0; --i) {
    //             parent_edge = mesh_hierarchy->ParentEntity(i, *parent_edge);
    //         }
    //         if(reader.IsPhysicalEntity(*parent_edge, inner_nr)) {
    //             // it is the inner boundary
    //             outer_boundary(*edge) = false;
    //         }
    //     }
    // }

    // auto inner_point{lf::mesh::utils::flagEntitiesOnBoundary(mesh, 2)};
    // // flag all nodes on the inner boundary
    // for(const lf::mesh::Entity* edge: mesh->Entities(1)) {
    //     if(outer_boundary(*edge)) {
    //         // mark the points associated with outer boundary edge to false
    //         for(const lf::mesh::Entity* subent: edge->SubEntities(1)) {
    //             inner_point(*subent) = false;
    //         }
    //     }
    // }

    // for(size_type dofnum = 0; dofnum < N_dofs; ++dofnum) {
    //     const lf::mesh::Entity &dof_node{dofh.Entity(dofnum)};
    //     const Eigen::Vector2d node_pos{lf::geometry::Corners(*dof_node.Geometry()).col(0)};
    //     if(inner_point(dof_node)) {
    //         std::cout << dofnum << ": [" << node_pos(0) << ", " 
    //               << node_pos(1) << "]" << std::endl;
    //     }
    // }

    for(int i = 0; i <= L; ++i) {
        auto mesh = mesh_hierarchy->getMesh(i);
        lf::assemble::UniformFEDofHandler dofh(mesh, {{lf::base::RefEl::kPoint(), 1}});
        const lf::assemble::size_type N_dofs(dofh.NumDofs());
        std::cout << "DofHandler(" << N_dofs << " dofs):" << std::endl;
        // output information about dofs for entities of all co-dimensions
        for(int codim = 0; codim <= mesh->DimMesh(); ++codim) {
            // visit all entities of a codimension codim
            for(const lf::mesh::Entity* e: mesh->Entities(codim)) {
                // Fetch unique index of current entity supplied by mesh object
                const lf::base::glb_idx_t e_idx = mesh->Index(*e);
                // Number of shape functions covering current entity
                const lf::assemble::size_type no_dofs(dofh.NumLocalDofs(*e));
                //obtain global indices of those shape functions
                nonstd::span<const lf::assemble::gdof_idx_t> dofarray{dofh.GlobalDofIndices(*e)};
                // print them

                if(codim == 2) {
                    const Eigen::Vector2d node_pos{lf::geometry::Corners(*(e->Geometry())).col(0)};
                    std::cout << *e << " " << e_idx << ": [" << node_pos(0) << ", " 
                              << node_pos(1) << "] " << no_dofs << " dofs = [";
                    for(int loc_dof_idx = 0; loc_dof_idx < no_dofs; ++loc_dof_idx) {
                        std::cout << dofarray[loc_dof_idx] << " ";
                    }
                    std::cout << " ]";
                } else {
                    std::cout << *e << " " << e_idx << ": " << no_dofs << " dofs = [";
                    for(int loc_dof_idx = 0; loc_dof_idx < no_dofs; ++loc_dof_idx) {
                        std::cout << dofarray[loc_dof_idx] << " ";
                    }
                    std::cout << " ]";
                }

                // Also output indices of interior shape functions
                nonstd::span<const lf::assemble::gdof_idx_t> intdofarray{dofh.InteriorGlobalDofIndices(*e)};
                std::cout << " int = [";
                for(lf::assemble::gdof_idx_t int_dof: intdofarray) {
                    std::cout << int_dof << " ";
                }
                std::cout << "]" << std::endl;
            }
        }
    }
    return 0;
}