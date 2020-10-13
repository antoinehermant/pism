// Copyright (C) 2004--2020 PISM Authors
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 3 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include "OptTillphiYieldStress.hh"
#include "MohrCoulombYieldStress.hh"
#include "MohrCoulombPointwise.hh"

#include "pism/util/IceGrid.hh"
#include "pism/util/Mask.hh"
#include "pism/util/error_handling.hh"
#include "pism/util/io/File.hh"
#include "pism/util/MaxTimestep.hh"
#include "pism/util/pism_utilities.hh"
#include "pism/util/Time.hh"
#include "pism/util/IceModelVec2CellType.hh"
#include "pism/geometry/Geometry.hh"
#include "pism/coupler/util/options.hh" // ForcingOptions

#include "pism/util/Time.hh"

namespace pism {

//! \file OptTillphiYieldStress.cc  Process model which computes pseudo-plastic yield stress for the subglacial layer.

/*! \file MohrCoulombYieldStress.cc
The output variable of this submodel is `tauc`, the pseudo-plastic yield stress
field that is used in the ShallowStressBalance objects.  This quantity is
computed by the Mohr-Coulomb criterion [\ref SchoofTill], but using an empirical
relation between the amount of water in the till and the effective pressure
of the overlying glacier resting on the till [\ref Tulaczyketal2000].

The "dry" strength of the till is a state variable which is protected to
the submodel, namely `tillphi`.  Its initialization is nontrivial: either the
`-topg_to_phi`  heuristic is used or inverse modeling can be used. (In the
latter case `tillphi` can be read-in at the beginning of the run.  

Currently `tillphi` is iteratively adjusted during the run, according to the misfit to a target surface elevation.

The effective pressure is derived from the till (pore) water amount (the effective water
layer thickness). Then the effective pressure is combined with tillphi to compute an
updated `tauc` by the Mohr-Coulomb criterion.

This submodel is inactive in floating areas.
*/


OptTillphiYieldStress::OptTillphiYieldStress(IceGrid::ConstPtr grid)
  : MohrCoulombYieldStress(grid) {

  m_name = "Mohr-Coulomb yield stress model to iteratively optimize till friction angle";

  //! Optimzation of till friction angle for given target surface elevation, analogous to
  // Pollard et al. (2012), TC 6(5), "A simple inverse method for the distribution of basal
  // sliding coefficients under ice sheets, applied to Antarctica"

  //m_iterative_phi = m_config->get_flag("basal_yield_stress.mohr_coulomb.iterative_phi.enabled");

  //if (m_iterative_phi) {

    m_usurf.create(m_grid, "usurf",
              WITH_GHOSTS);
    m_usurf.set_attrs("internal",
                 "surface elevation",
                 "m", "m", "surface_altitude", 0);

    m_target_usurf.create(m_grid, "target_usurf",
              WITH_GHOSTS);
    m_target_usurf.set_attrs("internal",
                 "target surface elevation",
                 "m", "m", "target_surface_altitude", 0);
    m_target_usurf.set_time_independent(true);

    m_diff_usurf.create(m_grid, "diff_usurf",
              WITH_GHOSTS);
    m_diff_usurf.set_attrs("internal",
                 "surface elevation anomaly",
                 "m", "m", "", 0);

    m_diff_mask.create(m_grid, "diff_mask",
              WITH_GHOSTS);
    m_diff_mask.set_attrs("internal",
                 "mask for till phi iteration",
                 "", "", "", 0);
  //} else {
  //  m_till_phi.set_time_independent(true);
  //}

  /*
  auto delta_file = m_config->get_string("basal_yield_stress.mohr_coulomb.delta.file");

  if (not delta_file.empty()) {
    ForcingOptions opt(*m_grid->ctx(), "basal_yield_stress.mohr_coulomb.delta");

    unsigned int buffer_size = m_config->get_number("input.forcing.buffer_size");
    unsigned int evaluations_per_year = m_config->get_number("input.forcing.evaluations_per_year");
    bool periodic = opt.period > 0;

    File file(m_grid->com, opt.filename, PISM_NETCDF3, PISM_READONLY);

    m_delta = IceModelVec2T::ForcingField(m_grid,
                                          file,
                                          "mohr_coulomb_delta",
                                          "", // no standard name
                                          buffer_size,
                                          evaluations_per_year,
                                          periodic, LINEAR);
    m_delta->set_attrs("", "minimum effective pressure on till as a fraction of overburden pressure",
                       "1", "1", "", 0);
  
  }
  */
}

OptTillphiYieldStress::~OptTillphiYieldStress() {
  // empty
}

/*
void OptTillphiYieldStress::restart_impl(const File &input_file, int record) {
  MohrCoulombYieldStress::restart_impl(input_file, record);
}
*/

//! Initialize the pseudo-plastic till mechanical model.
void OptTillphiYieldStress::bootstrap_impl(const File &input_file,
                                            const YieldStressInputs &inputs) {

  MohrCoulombYieldStress::bootstrap_impl(input_file, inputs);

  
  //Optimization scheme for till friction angle anlogous to Pollard et al. (2012) /////////
  m_iterative_phi = m_config->get_flag("basal_yield_stress.mohr_coulomb.iterative_phi.enabled");

  auto iterative_phi_file = m_config->get_string("basal_yield_stress.mohr_coulomb.iterative_phi.file");


  if (not iterative_phi_file.empty()) {

      m_log->message(2, "* Initializing the iterative till friction angle optimization...\n");

      m_usurf.regrid(iterative_phi_file, CRITICAL);
      m_target_usurf.copy_from(m_usurf);
      m_log->message(2, "* Read target surface elevation...\n");

  } else {

      m_log->message(2, "* No file set to read target surface elevation from... take '%s'\n", 
                           input_file.filename().c_str());
      m_usurf.regrid(input_file, CRITICAL);
      m_target_usurf.copy_from(m_usurf); 
  }

  dt_phi_inv = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.dt","seconds");

  double start_time = m_grid->ctx()->time()->start();
  m_last_time = start_time,
  m_last_inverse_time = start_time;


  iterative_phi_step(inputs.geometry->ice_surface_elevation,
                       inputs.geometry->bed_elevation,
                       inputs.geometry->cell_type);

  
  // regrid if requested, regardless of how initialized
  regrid("OptTillphiMohrCoulombYieldStress", m_till_phi);

  finish_initialization(inputs);
  
}

/*
void OptTillphiYieldStress::init_impl(const YieldStressInputs &inputs) {
  MohrCoulombYieldStress::init_impl(inputs);
}
*/
/*!
 * Finish initialization after bootstrapping or initializing using constants.
 */
/*
void OptTillphiYieldStress::finish_initialization(const YieldStressInputs &inputs) {
  MohrCoulombYieldStress::finish_initialization(inputs);
}
*/

MaxTimestep OptTillphiYieldStress::max_timestep_impl(double t) const {
  (void) t;
  
  if (m_delta) {
    auto dt = m_delta->max_timestep(t);

    if (dt.finite()) {
      return MaxTimestep(dt.value(), name());
    }
  }

  return MaxTimestep(name());
   
  //MohrCoulombYieldStress::max_timestep_impl(t);
  //FIXME: Add max timestep according to dt_phi_inv

}
/*
void OptTillphiYieldStress::set_till_friction_angle(const IceModelVec2S &input) {
  m_till_phi.copy_from(input);
}

void OptTillphiYieldStress::define_model_state_impl(const File &output) const {
  m_basal_yield_stress.define(output);
  m_till_phi.define(output);

}

void OptTillphiYieldStress::write_model_state_impl(const File &output) const {
  m_basal_yield_stress.write(output);
  m_till_phi.write(output);

}
*/
//! Update the till friction angle and the till yield stress for use in the pseudo-plastic
//! till basal stress model. See also IceBasalResistancePlasticLaw.

void OptTillphiYieldStress::update_impl(const YieldStressInputs &inputs,
                                         double t, double dt) {
  (void) t;
  (void) dt;

  //if (m_iterative_phi) {

    double dt_inverse = t - m_last_inverse_time;

    if (dt_inverse > dt_phi_inv) {

      iterative_phi_step(inputs.geometry->ice_surface_elevation,
                         inputs.geometry->bed_elevation,
                         inputs.geometry->cell_type);

      m_last_inverse_time = t;
    }
    m_last_time = t;
  //}

  MohrCoulombYieldStress::update_impl(inputs, t, dt);
  
}

void OptTillphiYieldStress::iterative_phi_step(const IceModelVec2S &ice_surface_elevation,
                                                const IceModelVec2S &bed_topography,
                                                const IceModelVec2CellType &mask) {

  //if (not m_config->get_flag("basal_yield_stress.mohr_coulomb.iterative_phi.enabled")) {
  //  throw RuntimeError::formatted(PISM_ERROR_LOCATION,
  //                                "basal_yield_stress.mohr_coulomb.iterative_phi.enabled is not set");
  //}

  m_log->message(2,"\n* Perform iterative step for optimization of till friction angle phi!\n\n");

  const IceModelVec2S &usurf = ice_surface_elevation;
  IceModelVec::AccessList list{&m_till_phi, &m_target_usurf, &m_diff_usurf, &m_diff_mask,
                                 &usurf, &bed_topography, &mask};

  m_diff_mask.set(1.0);

  const double
        h_inv = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.h_inv"),
    dhdt_conv = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.dh_conv"),
         dphi = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.dphi"),
      phi_min = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.phi_min"),
    phi_minup = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.phi_minup"),
     phi_max  = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.phi_max"),
     topg_min = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.topg_min"), 
     topg_max = m_config->get_number("basal_yield_stress.mohr_coulomb.iterative_phi.topg_max");

  double slope = (phi_minup - phi_min) / (topg_max - topg_min);

  m_log->message(2,
                 "  lower bound of till friction angle (phi) is piecewise-linear function of bed elev (topg):\n"
                 "             /  %5.2f                                         for   topg < %.f\n"
                 "   phi_min = |  %5.2f + (topg - (%.f)) * (%.2f / %.f)   for   %.f < topg < %.f\n"
                 "             \\  %5.2f                                        for   %.f < topg\n",
                 phi_min, topg_min,
                 phi_min, topg_min, phi_minup - phi_min, topg_max - topg_min, topg_min, topg_max,
                 phi_minup, topg_max);

  if (phi_min >= phi_max) {
    throw RuntimeError(PISM_ERROR_LOCATION,
                       "invalid -inverse_phi arguments: phi_min < phi_max is required");
  }

  if (topg_min >= topg_max) {
    throw RuntimeError(PISM_ERROR_LOCATION,
                       "invalid -invrse_phi arguments: topg_min < topg_max is required");
  }

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

      double diff_usurf_prev = m_diff_usurf(i,j);
      m_diff_usurf(i,j) = usurf(i,j)-m_target_usurf(i,j);
      double dh_step = std::abs(m_diff_usurf(i,j)-diff_usurf_prev);

      if (mask.grounded_ice(i,j)) {

        // Convergence criterion
        if (dh_step / dt_phi_inv > dhdt_conv) {
          m_diff_mask(i,j)=1.0;

          // Do incremental steps of maximum 0.5*dphi down and dphi up
          m_till_phi(i,j) -= std::min(dphi,std::max(-dphi*0.5,m_diff_usurf(i,j)/h_inv));

          // Different lower constraints for marine (b<topg_min) and continental (b>topg_max) areas)
          if (bed_topography(i,j) > topg_max){
            m_till_phi(i,j) = std::max(phi_minup,m_till_phi(i,j));

          // Apply smooth transition between marine and continental areas
          } else if (bed_topography(i,j) <= topg_max && bed_topography(i,j) >= topg_min){
            m_till_phi(i, j) = std::max((phi_min + (bed_topography(i,j) - topg_min) * slope),m_till_phi(i,j));

          // Apply absolute upper and lower bounds
          } else {
            m_till_phi(i,j) = std::max(phi_min,m_till_phi(i,j));
            m_till_phi(i,j) = std::min(phi_max,m_till_phi(i,j));
          }

        } else {
          m_diff_mask(i,j)=0.0;
        }

      // Floating and ice free ocean
      } else if (mask.ocean(i,j)){

        m_till_phi(i,j)   = phi_min;
        m_diff_mask(i,j)  = 0.0;
      }
  }
}


DiagnosticList OptTillphiYieldStress::diagnostics_impl() const {

  return combine({{"tillphi", Diagnostic::wrap(m_till_phi)},
                  {"diff_usurf", Diagnostic::wrap(m_diff_usurf)},
                  {"target_usurf", Diagnostic::wrap(m_target_usurf)},
                  {"diff_mask", Diagnostic::wrap(m_diff_mask)}},
                   YieldStress::diagnostics_impl());

}

} // end of namespace pism
