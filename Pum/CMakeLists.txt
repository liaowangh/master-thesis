set(pum_resolution_sources
  ../Pum_WaveRay/HE_FEM.h
  ../Pum_WaveRay/HE_FEM.cpp
  PUM_EdgeMat.h
  PUM_EdgeVector.h
  PUM_ElementMatrix.h
  PUM_ElemVector.h
  HE_PUM.h
  HE_PUM.cpp
  tests/pum_resolution.cpp
  ../utils/utils.h
  ../utils/utils.cpp
  ../utils/HE_solution.h
  ../utils/HE_solution.cpp
)

# define executable
add_executable(pum_resolution ${pum_resolution_sources})

#link executable with LehrFEM++
target_link_libraries(pum_resolution
  PUBLIC Eigen3::Eigen Boost::filesystem Boost::system Boost::program_options
  LF::lf.mesh
  LF::lf.mesh.utils
  LF::lf.mesh.test_utils
  LF::lf.mesh.hybrid2d
  LF::lf.refinement
  LF::lf.assemble
  LF::lf.io
  LF::lf.uscalfe
  LF::lf.base
)