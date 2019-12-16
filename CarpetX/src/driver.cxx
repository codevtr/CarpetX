#include "driver.hxx"
#include "io.hxx"
#include "prolongate_3d_rf2.hxx"
#include "schedule.hxx"

#include <cctk.h>
#include <cctk_Arguments.h>
#include <cctk_Parameters.h>
#include <util_Table.h>

#include <AMReX.H>
#include <AMReX_BCRec.H>
#include <AMReX_FillPatchUtil.H>
#include <AMReX_ParmParse.H>
#include <AMReX_PhysBCFunct.H>

#include <omp.h>
#include <mpi.h>

#include <cmath>
#include <memory>
#include <ostream>
#include <string>

namespace CarpetX {
using namespace amrex;
using namespace std;

// Global variables

int ghext_handle = -1;

amrex::AMReX *restrict pamrex = nullptr;
unique_ptr<GHExt> ghext;

// Registered functions

void *SetupGH(tFleshConfig *fc, int convLevel, cGH *cctkGH);
int InitGH(cGH *cctkGH);
int ScheduleTraverseGH(cGH *cctkGH, const char *where);

int MyProc(const cGH *cctkGH);
int nProcs(const cGH *cctkGH);
int Exit(cGH *cctkGH, int retval);
int Abort(cGH *cctkGH, int retval);
int Barrier(const cGH *cctkGHa);

// Aliased functions

extern "C" void CarpetX_CallScheduleGroup(void *cctkGH, const char *groupname);

// Local functions
void SetupLevel(int level, const BoxArray &ba, const DistributionMapping &dm);
void SetupGlobals();

////////////////////////////////////////////////////////////////////////////////

// AmrCore functions

CactusAmrCore::CactusAmrCore() {}
CactusAmrCore::CactusAmrCore(const RealBox *rb, int max_level_in,
                             const Vector<int> &n_cell_in, int coord,
                             Vector<IntVect> ref_ratios, const int *is_per)
    : AmrCore(rb, max_level_in, n_cell_in, coord, ref_ratios, is_per) {
  SetupGlobals();
}

CactusAmrCore::CactusAmrCore(const RealBox &rb, int max_level_in,
                             const Vector<int> &n_cell_in, int coord,
                             Vector<IntVect> const &ref_ratios,
                             Array<int, AMREX_SPACEDIM> const &is_per)
    : AmrCore(rb, max_level_in, n_cell_in, coord, ref_ratios, is_per) {
  SetupGlobals();
}

CactusAmrCore::~CactusAmrCore() {}

void CactusAmrCore::ErrorEst(const int level, TagBoxArray &tags, Real time,
                             int ngrow) {
  DECLARE_CCTK_PARAMETERS;

  // Don't regrid before Cactus is ready to
  if (level >= int(ghext->leveldata.size()))
    return;

  if (verbose)
    CCTK_VINFO("ErrorEst level %d", level);

  const int gi = CCTK_GroupIndex("CarpetX::regrid_error");
  assert(gi >= 0);
  const int vi = 0;
  const int tl = 0;

  auto &restrict leveldata = ghext->leveldata.at(level);
  auto &restrict groupdata = *leveldata.groupdata.at(gi);
  // Ensure the error estimate has been set
  assert(groupdata.valid.at(tl).at(vi).valid_int);
  auto mfitinfo = MFItInfo().SetDynamic(true).EnableTiling(
      {max_tile_size_x, max_tile_size_y, max_tile_size_z});
#pragma omp parallel
  for (MFIter mfi(*leveldata.mfab0, mfitinfo); mfi.isValid(); ++mfi) {
    GridPtrDesc1 grid(leveldata, groupdata, mfi);

    const Array4<const CCTK_REAL> &err_array4 =
        groupdata.mfab.at(tl)->array(mfi);
    const GF3D1<const CCTK_REAL> err_ = grid.gf3d(err_array4, vi);
    const Array4<char> &tags_array4 = tags.array(mfi);

    grid.loop_idx(
        where_t::interior, groupdata.indextype, [&](const Loop::PointDesc &p) {
          tags_array4(grid.cactus_offset.x + p.i, grid.cactus_offset.y + p.j,
                      grid.cactus_offset.z + p.k) =
              err_(p.I) >= regrid_error_threshold ? TagBox::SET : TagBox::CLEAR;
        });
    // Do not set the boundary; AMReX's error grid function might have
    // a different number of ghost zones, and these ghost zones are
    // probably unused anyway.
    if (false) {
      grid.loop_idx(where_t::boundary, groupdata.indextype,
                    [&](const Loop::PointDesc &p) {
                      tags_array4(grid.cactus_offset.x + p.i,
                                  grid.cactus_offset.y + p.j,
                                  grid.cactus_offset.z + p.k) = TagBox::CLEAR;
                    });
    }
  }
}

void SetupGlobals() {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("SetupGlobals");

  GHExt::GlobalData &globaldata = ghext->globaldata;

  const int numgroups = CCTK_NumGroups();
  globaldata.scalargroupdata.resize(numgroups);
  for (int gi = 0; gi < numgroups; ++gi) {
    cGroup group;
    int ierr = CCTK_GroupData(gi, &group);
    assert(!ierr);

    /* only grid functions live on levels (and the grid) */
    if (group.grouptype != CCTK_SCALAR and group.grouptype != CCTK_ARRAY)
      continue;

    assert(group.grouptype == CCTK_SCALAR);
    assert(group.vartype == CCTK_VARIABLE_REAL);
    assert(group.disttype == CCTK_DISTRIB_CONSTANT);
    assert(group.dim == 0);

    globaldata.scalargroupdata.at(gi) =
      make_unique<GHExt::GlobalData::ScalarGroupData>();
    GHExt::GlobalData::ScalarGroupData &scalargroupdata =
      *globaldata.scalargroupdata.at(gi);
    scalargroupdata.groupindex = gi;
    scalargroupdata.firstvarindex = CCTK_FirstVarIndexI(gi);
    scalargroupdata.numvars = group.numvars;

    // Allocate data
    scalargroupdata.data.resize(group.numtimelevels);
    scalargroupdata.valid.resize(group.numtimelevels);
    for (int tl = 0; tl < int(scalargroupdata.data.size()); ++tl) {
      scalargroupdata.data.at(tl).resize(scalargroupdata.numvars);
      scalargroupdata.valid.at(tl).resize(scalargroupdata.numvars);
      for (int vi = 0; vi < scalargroupdata.numvars; ++vi) {
        // TODO: decide that valid_bnd == false always and rely on
        // initialization magic?
        scalargroupdata.valid.at(tl).at(vi).valid_int = false;
        scalargroupdata.valid.at(tl).at(vi).valid_bnd = true;

        // TODO: make poison_invalid and check_invalid virtual members of
        // CommonGroupData
        poison_invalid(scalargroupdata, vi, tl);
        check_valid(scalargroupdata, vi, tl, [&]() { return "SetupGlobals"; });
      }
    }
  }
}

array<int, dim> get_group_indextype(const int gi) {
  assert(gi >= 0);
  const int tags = CCTK_GroupTagsTableI(gi);
  assert(tags >= 0);
  array<CCTK_INT, dim> index;
  int iret = Util_TableGetIntArray(tags, dim, index.data(), "index");
  if (iret == UTIL_ERROR_TABLE_NO_SUCH_KEY) {
    index = {1, 1, 1}; // default: cell-centred
  } else if (iret >= 0) {
    assert(iret == dim);
  } else {
    assert(0);
  }
  array<int, dim> indextype;
  for (int d = 0; d < dim; ++d)
    indextype[d] = index[d];
  return indextype;
}

array<int, dim> get_group_fluxes(const int gi) {
  assert(gi >= 0);
  const int tags = CCTK_GroupTagsTableI(gi);
  assert(tags >= 0);
  vector<char> fluxes_buf(1000);
  const int iret =
      Util_TableGetString(tags, fluxes_buf.size(), fluxes_buf.data(), "fluxes");
  if (iret == UTIL_ERROR_TABLE_NO_SUCH_KEY) {
    fluxes_buf[0] = '\0'; // default: empty (no fluxes)
  } else if (iret >= 0) {
    // do nothing
  } else {
    assert(0);
  }

  const string str(fluxes_buf.data());
  vector<string> strs;
  size_t end = 0;
  while (end < str.size()) {
    size_t begin = str.find_first_not_of(' ', end);
    if (begin == string::npos)
      break;
    end = str.find(' ', begin);
    strs.push_back(str.substr(begin, end - begin));
  }

  array<int, dim> fluxes;
  fluxes.fill(-1);
  if (strs.empty())
    return fluxes; // No fluxes specified

  assert(strs.size() == dim); // Check number of fluxes
  for (int d = 0; d < dim; ++d) {
    auto str1 = strs[d];
    if (str1.find(':') == string::npos) {
      const char *impl = CCTK_GroupImplementationI(gi);
      str1 = string(impl) + "::" + str1;
    }
    int gi1 = CCTK_GroupIndex(str1.c_str());
    assert(gi1 >= 0); // Check fluxes are valid groups
    fluxes[d] = gi1;
  }

  for (int d = 0; d < dim; ++d)
    for (int d1 = d + 1; d1 < dim; ++d1)
      assert(fluxes[d] != fluxes[d1]); // Check groups are all different
  return fluxes;
}

array<int, dim> get_group_nghostzones(const int gi) {
  DECLARE_CCTK_PARAMETERS;
  assert(gi >= 0);
  const int tags = CCTK_GroupTagsTableI(gi);
  assert(tags >= 0);
  array<CCTK_INT, dim> nghostzones;
  int iret =
      Util_TableGetIntArray(tags, dim, nghostzones.data(), "nghostzones");
  if (iret == UTIL_ERROR_TABLE_NO_SUCH_KEY) {
    // default: use driver parameter
    nghostzones = {ghost_size, ghost_size, ghost_size};
  } else if (iret >= 0) {
    assert(iret == dim);
  } else {
    assert(0);
  }
  return nghostzones;
}

void SetupLevel(int level, const BoxArray &ba, const DistributionMapping &dm) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("SetupLevel level %d", level);

  assert(level == int(ghext->leveldata.size()));
  ghext->leveldata.resize(level + 1);
  GHExt::LevelData &leveldata = ghext->leveldata.at(level);
  leveldata.level = level;
  // TODO: Make this an empty MultiFab
  leveldata.mfab0 = make_unique<MultiFab>(ba, dm, 1, ghost_size);
  assert(ba.ixType() ==
         IndexType(IndexType::CELL, IndexType::CELL, IndexType::CELL));

  const int numgroups = CCTK_NumGroups();
  leveldata.groupdata.resize(numgroups);
  for (int gi = 0; gi < numgroups; ++gi) {
    cGroup group;
    int ierr = CCTK_GroupData(gi, &group);
    assert(!ierr);

    /* only grid functions live on levels (and the grid) */
    if (group.grouptype != CCTK_GF)
      continue;

    assert(group.grouptype == CCTK_GF);
    assert(group.vartype == CCTK_VARIABLE_REAL);
    assert(group.disttype == CCTK_DISTRIB_DEFAULT);
    assert(group.dim == dim);

    leveldata.groupdata.at(gi) = make_unique<GHExt::LevelData::GroupData>();
    GHExt::LevelData::GroupData &groupdata = *leveldata.groupdata.at(gi);
    groupdata.groupindex = gi;
    groupdata.firstvarindex = CCTK_FirstVarIndexI(gi);
    groupdata.numvars = group.numvars;
    groupdata.indextype = get_group_indextype(gi);
    groupdata.nghostzones = get_group_nghostzones(gi);

    // Allocate grid hierarchies
    const BoxArray &gba = convert(
        ba,
        IndexType(groupdata.indextype[0] ? IndexType::CELL : IndexType::NODE,
                  groupdata.indextype[1] ? IndexType::CELL : IndexType::NODE,
                  groupdata.indextype[2] ? IndexType::CELL : IndexType::NODE));
    groupdata.mfab.resize(group.numtimelevels);
    groupdata.valid.resize(group.numtimelevels);
    for (int tl = 0; tl < int(groupdata.mfab.size()); ++tl) {
      groupdata.mfab.at(tl) = make_unique<MultiFab>(
          gba, dm, groupdata.numvars, IntVect(groupdata.nghostzones));
      groupdata.valid.at(tl) = vector<valid_t>(groupdata.numvars);
      for (int vi = 0; vi < groupdata.numvars; ++vi)
        poison_invalid(leveldata, groupdata, vi, tl);
    }

    if (level > 0) {
      array<int, dim> fluxes = get_group_fluxes(groupdata.groupindex);
      const bool have_fluxes = fluxes[0] >= 0;
      if (have_fluxes) {
        assert((groupdata.indextype == array<int, dim>{1, 1, 1}));
        groupdata.freg = make_unique<FluxRegister>(
            gba, dm, ghext->amrcore->refRatio(level - 1), level,
            groupdata.numvars);
        groupdata.fluxes = fluxes;
      } else {
        groupdata.fluxes.fill(-1);
      }
    }
  }

  // Check flux register consistency
  for (int gi = 0; gi < numgroups; ++gi) {
    cGroup group;
    int ierr = CCTK_GroupData(gi, &group);
    assert(!ierr);

    /* only grid functions live on levels (and the grid) */
    if (group.grouptype != CCTK_GF)
      continue;
    const auto &groupdata = *leveldata.groupdata.at(gi);
    if (groupdata.freg) {
      for (int d = 0; d < dim; ++d) {
        assert(groupdata.fluxes[d] != groupdata.groupindex);
        const auto &flux_groupdata =
            *leveldata.groupdata.at(groupdata.fluxes[d]);
        array<int, dim> flux_indextype{1, 1, 1};
        flux_indextype[d] = 0;
        assert(flux_groupdata.indextype == flux_indextype);
        assert(flux_groupdata.numvars == groupdata.numvars);
      }
    }
  }
}

Interpolater *get_interpolator(const array<int, dim> indextype) {
  DECLARE_CCTK_PARAMETERS;

  enum class interp_t { unset, interpolate, conservative, ddf };
  static interp_t interp = interp_t::unset;
  if (interp == interp_t::unset) {
    if (CCTK_EQUALS(prolongation_type, "interpolate")) {
      interp = interp_t::interpolate;
    } else if (CCTK_EQUALS(prolongation_type, "conservative")) {
      interp = interp_t::conservative;
    } else if (CCTK_EQUALS(prolongation_type, "ddf")) {
      interp = interp_t::ddf;
    } else {
      assert(0);
    }
  }
  assert(interp != interp_t::unset);

  switch (interp) {
  case interp_t::interpolate:

    switch ((indextype[0] << 2) | (indextype[1] << 1) | (indextype[2] << 0)) {

    case 0b000:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c000_o1;
      case 3:
        return &prolongate_3d_rf2_c000_o3;
      }
      break;

    case 0b001:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c001_o1;
      case 3:
        return &prolongate_3d_rf2_c001_o3;
      }
      break;

    case 0b010:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c010_o1;
      case 3:
        return &prolongate_3d_rf2_c010_o3;
      }
      break;

    case 0b011:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c011_o1;
      case 3:
        return &prolongate_3d_rf2_c011_o3;
      }
      break;

    case 0b100:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c100_o1;
      case 3:
        return &prolongate_3d_rf2_c100_o3;
      }
      break;

    case 0b101:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c101_o1;
      case 3:
        return &prolongate_3d_rf2_c101_o3;
      }
      break;

    case 0b110:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c110_o1;
      case 3:
        return &prolongate_3d_rf2_c110_o3;
      }
      break;

    case 0b111:
      switch (prolongation_order) {
      case 1:
        return &prolongate_3d_rf2_c111_o1;
      case 3:
        return &prolongate_3d_rf2_c111_o3;
      }
      break;
    }
    break;

  case interp_t::conservative:

    switch ((indextype[0] << 2) | (indextype[1] << 1) | (indextype[2] << 0)) {

    case 0b000:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c000_o0;
      }
      break;

    case 0b001:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c001_o0;
      }
      break;

    case 0b010:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c010_o0;
      }
      break;

    case 0b011:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c011_o0;
      }
      break;

    case 0b100:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c100_o0;
      }
      break;

    case 0b101:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c101_o0;
      }
      break;

    case 0b110:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c110_o0;
      }
      break;

    case 0b111:
      switch (prolongation_order) {
      case 1:
        return &prolongate_cons_3d_rf2_c111_o0;
      }
      break;
    }
    break;

  case interp_t::ddf:

    switch ((indextype[0] << 2) | (indextype[1] << 1) | (indextype[2] << 0)) {
    case 0b000:
      return &prolongate_ddf_3d_rf2_c000_o1;
    case 0b001:
      return &prolongate_ddf_3d_rf2_c001_o1;
    case 0b010:
      return &prolongate_ddf_3d_rf2_c010_o1;
    case 0b011:
      return &prolongate_ddf_3d_rf2_c011_o1;
    case 0b100:
      return &prolongate_ddf_3d_rf2_c100_o1;
    case 0b101:
      return &prolongate_ddf_3d_rf2_c101_o1;
    case 0b110:
      return &prolongate_ddf_3d_rf2_c110_o1;
    case 0b111:
      return &prolongate_ddf_3d_rf2_c111_o1;
    }
    break;

  case interp_t::unset:
    // do nothing; errors are handled below
    break;

  } // switch prolongation_type

  CCTK_VERROR("Unsupported combination of prolongation_type \"%s\", "
              "prolongation order %d, and index type [%d,%d,%d]",
              prolongation_type, prolongation_order, indextype[0], indextype[1],
              indextype[2]);
  assert(0);
}

void CactusAmrCore::MakeNewLevelFromScratch(int level, Real time,
                                            const BoxArray &ba,
                                            const DistributionMapping &dm) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("MakeNewLevelFromScratch level %d", level);

  SetupLevel(level, ba, dm);

  if (saved_cctkGH) {
    assert(current_level == -1);
    current_level = level;
    CCTK_Traverse(saved_cctkGH, "CCTK_BASEGRID");
    // CCTK_Traverse(saved_cctkGH, "CCTK_POSTREGRID");
    current_level = -1;
  }
}

void CactusAmrCore::MakeNewLevelFromCoarse(int level, Real time,
                                           const BoxArray &ba,
                                           const DistributionMapping &dm) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("MakeNewLevelFromCoarse level %d", level);
  assert(level > 0);

  SetupLevel(level, ba, dm);

  // Prolongate
  auto &leveldata = ghext->leveldata.at(level);
  auto &coarseleveldata = ghext->leveldata.at(level - 1);
  const int num_groups = CCTK_NumGroups();
  for (int gi = 0; gi < num_groups; ++gi) {
    cGroup group;
    int ierr = CCTK_GroupData(gi, &group);
    assert(!ierr);

    if (group.grouptype != CCTK_GF)
      continue;

    auto &restrict groupdata = *leveldata.groupdata.at(gi);
    auto &restrict coarsegroupdata = *coarseleveldata.groupdata.at(gi);
    assert(coarsegroupdata.numvars == groupdata.numvars);
    Interpolater *const interpolator = get_interpolator(groupdata.indextype);
    PhysBCFunctNoOp cphysbc;
    PhysBCFunctNoOp fphysbc;
    const IntVect reffact{2, 2, 2};
    // boundary conditions
    const BCRec bcrec(periodic_x || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_y || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_z || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_x || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_y || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_z || periodic ? BCType::int_dir : BCType::reflect_odd);
    const Vector<BCRec> bcs(groupdata.numvars, bcrec);

    // If there is more than one time level, then we don't prolongate
    // the oldest.
    int ntls = groupdata.mfab.size();
    int prolongate_tl = ntls > 1 ? ntls - 1 : ntls;
    for (int tl = 0; tl < ntls; ++tl)
      groupdata.valid.at(tl) = vector<valid_t>(groupdata.numvars);
    for (int tl = 0; tl < prolongate_tl; ++tl) {
      // Only interpolate if coarse grid data are valid
      bool all_invalid = true;
      for (int vi = 0; vi < groupdata.numvars; ++vi)
        all_invalid &= !coarsegroupdata.valid.at(tl).at(vi).valid_int &&
                       !coarsegroupdata.valid.at(tl).at(vi).valid_bnd;
      if (all_invalid) {
        for (int vi = 0; vi < groupdata.numvars; ++vi) {
          groupdata.valid.at(tl).at(vi).valid_int = false;
          groupdata.valid.at(tl).at(vi).valid_bnd = false;
        }
      } else {
        // Expext coarse grid data to be valid
        for (int vi = 0; vi < groupdata.numvars; ++vi) {
          if (!(coarsegroupdata.valid.at(tl).at(vi).valid_int &&
                coarsegroupdata.valid.at(tl).at(vi).valid_bnd)) {
            valid_t all_valid;
            all_valid.valid_int = true;
            all_valid.valid_bnd = true;
            CCTK_VERROR("MakeNewLevelFromCoarse before prolongation: Grid "
                        "function \"%s\" is invalid on refinement level %d, "
                        "time level %d; expected valid %s, found valid %s",
                        CCTK_FullVarName(coarsegroupdata.firstvarindex + vi),
                        coarseleveldata.level, tl, string(all_valid).c_str(),
                        string(coarsegroupdata.valid.at(tl).at(vi)).c_str());
          }
        }
        for (int vi = 0; vi < groupdata.numvars; ++vi) {
          check_valid(coarseleveldata, coarsegroupdata, vi, tl, [&]() {
            return "MakeNewLevelFromCoarse before prolongation";
          });
        }
        InterpFromCoarseLevel(
            *groupdata.mfab.at(tl), 0.0, *coarsegroupdata.mfab.at(tl), 0, 0,
            groupdata.numvars, ghext->amrcore->Geom(level - 1),
            ghext->amrcore->Geom(level), cphysbc, 0, fphysbc, 0, reffact,
            interpolator, bcs, 0);
        for (int vi = 0; vi < groupdata.numvars; ++vi) {
          groupdata.valid.at(tl).at(vi).valid_int =
              coarsegroupdata.valid.at(tl).at(vi).valid_int &&
              coarsegroupdata.valid.at(tl).at(vi).valid_bnd;
          groupdata.valid.at(tl).at(vi).valid_bnd = false;
        }
      }
      for (int vi = 0; vi < groupdata.numvars; ++vi) {
        poison_invalid(leveldata, groupdata, vi, tl);
        check_valid(leveldata, groupdata, vi, tl, [&]() {
          return "MakeNewLevelFromCoarse after prolongation";
        });
      }
    }
  }

  if (saved_cctkGH) {
    assert(current_level == -1);
    current_level = level;
    CCTK_Traverse(saved_cctkGH, "CCTK_BASEGRID");
    CCTK_Traverse(saved_cctkGH, "CCTK_POSTREGRID");
    current_level = -1;
  }
}

void CactusAmrCore::RemakeLevel(int level, Real time, const BoxArray &ba,
                                const DistributionMapping &dm) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("RemakeLevel level %d", level);

  // Copy or prolongate
  auto &leveldata = ghext->leveldata.at(level);
  // TODO: Make this an empty MultiFab
  leveldata.mfab0 = make_unique<MultiFab>(ba, dm, 1, ghost_size);
  assert(ba.ixType() ==
         IndexType(IndexType::CELL, IndexType::CELL, IndexType::CELL));

  const int num_groups = CCTK_NumGroups();
  for (int gi = 0; gi < num_groups; ++gi) {
    cGroup group;
    int ierr = CCTK_GroupData(gi, &group);
    assert(!ierr);

    if (group.grouptype != CCTK_GF)
      continue;

    auto &restrict groupdata = *leveldata.groupdata.at(gi);

    const BoxArray &gba = convert(
        ba,
        IndexType(groupdata.indextype[0] ? IndexType::CELL : IndexType::NODE,
                  groupdata.indextype[1] ? IndexType::CELL : IndexType::NODE,
                  groupdata.indextype[2] ? IndexType::CELL : IndexType::NODE));

    // If there is more than one time level, then we don't
    // prolongate the oldest.
    int ntls = groupdata.mfab.size();
    int prolongate_tl = ntls > 1 ? ntls - 1 : ntls;

    // This must not happen
    assert(leveldata.level > 0);

    // Copy from same level and/or prolongate from next coarser level
    auto &coarseleveldata = ghext->leveldata.at(level - 1);
    auto &restrict coarsegroupdata = *coarseleveldata.groupdata.at(gi);
    assert(coarsegroupdata.numvars == groupdata.numvars);
    Interpolater *const interpolator = get_interpolator(groupdata.indextype);

    PhysBCFunctNoOp cphysbc;
    PhysBCFunctNoOp fphysbc;
    const IntVect reffact{2, 2, 2};
    // boundary conditions
    const BCRec bcrec(periodic_x || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_y || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_z || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_x || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_y || periodic ? BCType::int_dir : BCType::reflect_odd,
                      periodic_z || periodic ? BCType::int_dir : BCType::reflect_odd);
    const Vector<BCRec> bcs(groupdata.numvars, bcrec);
    for (int tl = 0; tl < ntls; ++tl) {
      auto mfab = make_unique<MultiFab>(gba, dm, groupdata.numvars,
                                        IntVect(groupdata.nghostzones));
      auto valid = vector<valid_t>(groupdata.numvars);
      if (poison_undefined_values) {
        // Set new grid functions to nan
        auto mfitinfo = MFItInfo().SetDynamic(true).EnableTiling(
            {max_tile_size_x, max_tile_size_y, max_tile_size_z});
#pragma omp parallel
        for (MFIter mfi(*leveldata.mfab0, mfitinfo); mfi.isValid(); ++mfi) {
          GridPtrDesc1 grid(leveldata, groupdata, mfi);
          const Array4<CCTK_REAL> &vars = mfab->array(mfi);
          for (int vi = 0; vi < groupdata.numvars; ++vi) {
            const GF3D1<CCTK_REAL> ptr_ = grid.gf3d(vars, vi);
            grid.loop_idx(
                where_t::everywhere, groupdata.indextype, groupdata.nghostzones,
                [&](const Loop::PointDesc &p) { ptr_(p.I) = 0.0 / 0.0; });
          }
        }
      }
      if (tl < prolongate_tl) {
        // Only interpolate if coarse grid data are valid
        bool all_invalid = true;
        for (int vi = 0; vi < groupdata.numvars; ++vi)
          all_invalid &= !coarsegroupdata.valid.at(tl).at(vi).valid_int &&
                         !coarsegroupdata.valid.at(tl).at(vi).valid_bnd &&
                         !groupdata.valid.at(tl).at(vi).valid_int &&
                         !groupdata.valid.at(tl).at(vi).valid_bnd;

        if (all_invalid) {
          // do nothing
        } else {
          for (int vi = 0; vi < groupdata.numvars; ++vi) {
            const bool cond = coarsegroupdata.valid.at(tl).at(vi).valid_int &&
                              coarsegroupdata.valid.at(tl).at(vi).valid_bnd &&
                              groupdata.valid.at(tl).at(vi).valid_int &&
                              groupdata.valid.at(tl).at(vi).valid_bnd;
            if (!cond)
              CCTK_VERROR("Found invalid input data: RemakeLevel level %d, "
                          "variable %s%s: need everything defined, have coarse "
                          "%s, have current %s",
                          leveldata.level,
                          CCTK_FullVarName(groupdata.firstvarindex + vi),
                          string("_p", tl).c_str(),
                          string(coarsegroupdata.valid.at(tl).at(vi)).c_str(),
                          string(groupdata.valid.at(tl).at(vi)).c_str());
            check_valid(coarseleveldata, coarsegroupdata, vi, tl,
                        [&]() { return "RemakeLevel before prolongation"; });
            // We cannot call this function since it would try to
            // traverse the old grid function with the new grid
            // structure.
            // TODO: Use explicit loop instead (see above)
            // check_valid(leveldata, groupdata, vi, tl);
          }

          FillPatchTwoLevels(*mfab, 0.0, {&*coarsegroupdata.mfab.at(tl)}, {0.0},
                             {&*groupdata.mfab.at(tl)}, {0.0}, 0, 0,
                             groupdata.numvars, ghext->amrcore->Geom(level - 1),
                             ghext->amrcore->Geom(level), cphysbc, 0, fphysbc,
                             0, reffact, interpolator, bcs, 0);

          for (int vi = 0; vi < groupdata.numvars; ++vi) {
            valid.at(vi).valid_int =
                coarsegroupdata.valid.at(tl).at(vi).valid_int &&
                coarsegroupdata.valid.at(tl).at(vi).valid_bnd &&
                groupdata.valid.at(tl).at(vi).valid_int &&
                groupdata.valid.at(tl).at(vi).valid_bnd;
            valid.at(vi).valid_bnd = false;
          }
        }
      }
      groupdata.mfab.at(tl) = move(mfab);
      groupdata.valid.at(tl) = move(valid);

      if (groupdata.freg)
        groupdata.freg = make_unique<FluxRegister>(
            gba, dm, ghext->amrcore->refRatio(level - 1), level,
            groupdata.numvars);

      for (int vi = 0; vi < groupdata.numvars; ++vi) {
        poison_invalid(leveldata, groupdata, vi, tl);
        check_valid(leveldata, groupdata, vi, tl,
                    [&]() { return "RemakeLevel after prolongation"; });
      }
    }
  }

  if (saved_cctkGH) {
    assert(current_level == -1);
    current_level = level;
    CCTK_Traverse(saved_cctkGH, "CCTK_BASEGRID");
    CCTK_Traverse(saved_cctkGH, "CCTK_POSTREGRID");
    current_level = -1;
  }
}

void CactusAmrCore::ClearLevel(int level) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("ClearLevel level %d", level);

  // assert(level == int(ghext->leveldata.size()) - 1);
  ghext->leveldata.resize(level);
}

////////////////////////////////////////////////////////////////////////////////

// Start driver
extern "C" int CarpetX_Startup() {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("Startup");

  // Output a startup message
  string banner = "AMR driver provided by CarpetX, using AMReX " +
                  amrex::Version() +
                  " ("
#ifdef AMREX_USE_MPI
                  "MPI, "
#else
                  "no MPI, "
#endif
#ifdef AMREX_USE_OMP
                  "OpenMP, "
#else
                  "no OpenMP, "
#endif
#ifdef AMREX_USE_GPU
                  "Accelerators, "
#else
                  "no Accelerators, "
#endif
#ifdef AMREX_USE_ASSERTION
                  "DEBUG, "
#else
                  "OPTIMIZED, "
#endif
                  ")";
  int ierr = CCTK_RegisterBanner(banner.c_str());
  assert(!ierr);

  // Register a GH extension
  ghext_handle = CCTK_RegisterGHExtension("CarpetX");
  assert(ghext_handle >= 0);
  int iret = CCTK_RegisterGHExtensionSetupGH(ghext_handle, SetupGH);
  assert(iret);
  iret = CCTK_RegisterGHExtensionInitGH(ghext_handle, InitGH);
  assert(iret);
  iret = CCTK_RegisterGHExtensionScheduleTraverseGH(ghext_handle,
                                                    ScheduleTraverseGH);
  assert(iret);

  CCTK_OverloadInitialise(Initialise);
  CCTK_OverloadEvolve(Evolve);
  CCTK_OverloadShutdown(Shutdown);
  CCTK_OverloadOutputGH(OutputGH);

  CCTK_OverloadMyProc(MyProc);
  CCTK_OverloadnProcs(nProcs);
  CCTK_OverloadExit(Exit);
  CCTK_OverloadAbort(Abort);
  CCTK_OverloadBarrier(Barrier);

  CCTK_OverloadEnableGroupStorage(EnableGroupStorage);
  CCTK_OverloadDisableGroupStorage(DisableGroupStorage);
  CCTK_OverloadGroupStorageIncrease(GroupStorageIncrease);
  CCTK_OverloadGroupStorageDecrease(GroupStorageDecrease);

  CCTK_OverloadSyncGroupsByDirI(SyncGroupsByDirI);

  return 0;
}

// Set up GH extension
void *SetupGH(tFleshConfig *fc, int convLevel, cGH *restrict cctkGH) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("SetupGH");

  assert(fc);
  assert(convLevel == 0);
  assert(cctkGH);

  // Initialize AMReX
  ParmParse pp;
  // Don't catch Unix signals. If signals are caught, we don't get
  // core files.
  pp.add("amrex.signal_handling", 0);
  // Throw exceptions for failing AMReX assertions. With exceptions,
  // we get core files.
  pp.add("amrex.throw_exception", 1);
  pamrex = amrex::Initialize(MPI_COMM_WORLD);

  // Create grid structure
  ghext = make_unique<GHExt>();

  return ghext.get();
}

// Initialize GH extension
int InitGH(cGH *restrict cctkGH) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("InitGH");

  assert(cctkGH);

  // Domain
  const RealBox domain({xmin, ymin, zmin}, {xmax, ymax, zmax});

  // Number of coarse grid cells
  const Vector<int> ncells{ncells_x, ncells_y, ncells_z};

  const int coord = -1; // undefined?

  // Refinement ratios
  const Vector<IntVect> reffacts; // empty

  // Periodicity
  const Array<int, dim> is_periodic{periodic_x || periodic, periodic_y || periodic, periodic_z || periodic};

  // Set blocking factors via parameter table since AmrMesh needs to
  // know them when its constructor is running, but there are no
  // constructor arguments for them
  ParmParse pp;
  pp.add("amr.blocking_factor_x", blocking_factor_x);
  pp.add("amr.blocking_factor_y", blocking_factor_y);
  pp.add("amr.blocking_factor_z", blocking_factor_z);
  pp.add("amr.max_grid_size_x", max_grid_size_x);
  pp.add("amr.max_grid_size_y", max_grid_size_y);
  pp.add("amr.max_grid_size_z", max_grid_size_z);
  pp.add("amr.grid_eff", grid_efficiency);

  ghext->amrcore = make_unique<CactusAmrCore>(
      domain, max_num_levels - 1, ncells, coord, reffacts, is_periodic);

  if (verbose) {
    int maxnumlevels = ghext->amrcore->maxLevel() + 1;
    for (int level = 0; level < maxnumlevels; ++level) {
      CCTK_VINFO("Geometry level %d:", level);
      cout << ghext->amrcore->Geom(level) << "\n";
    }
  }

  // CCTK_VINFO("BoxArray level %d:", level);
  // cout << ghext->amrcore->boxArray(level) << "\n";
  // CCTK_VINFO("DistributionMap level %d:", level);
  // cout << ghext->amrcore->DistributionMap(level) << "\n";

  return 0; // unused
}

// Traverse schedule
int ScheduleTraverseGH(cGH *restrict cctkGH, const char *where) {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("ScheduleTraverseGH iteration %d %s", cctkGH->cctk_iteration,
               where);

  int ierr = CCTK_ScheduleTraverse(where, cctkGH, CallFunction);
  assert(!ierr);

  return 0; // unused
}

// Shut down driver
extern "C" int CarpetX_Shutdown() {
  DECLARE_CCTK_PARAMETERS;
  if (verbose)
    CCTK_VINFO("Shutdown");

  // Should we really do this? Cactus's extension handling mechanism
  // becomes inconsistent once extensions have been unregistered.
  int iret = CCTK_UnregisterGHExtension("CarpetX");
  assert(iret == 0);

  // Deallocate grid hierarchy
  ghext = nullptr;

  // Finalize AMReX
  amrex::Finalize(pamrex);
  pamrex = nullptr;

  return 0;
}

////////////////////////////////////////////////////////////////////////////////

int MyProc(const cGH *restrict cctkGH) { return ParallelDescriptor::MyProc(); }

int nProcs(const cGH *restrict cctkGH) { return ParallelDescriptor::NProcs(); }

int Exit(cGH *cctkGH, int retval) {
  ParallelDescriptor::Abort(retval);
  return 0; // unreachable
}

int Abort(cGH *cctkGH, int retval) {
  ParallelDescriptor::Abort(retval);
  return 0; // unreachable
}

int Barrier(const cGH *restrict cctkGH) {
  ParallelDescriptor::Barrier();
  return 0;
}

void CarpetX_CallScheduleGroup(void *cctkGH_, const char *groupname) {
  cGH *cctkGH = static_cast<cGH *>(cctkGH_);
  int ierr = CCTK_ScheduleTraverse(groupname, cctkGH, CallFunction);
  assert(!ierr);
}

} // namespace CarpetX