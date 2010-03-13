// Copyright (C) 2004-2010 Jed Brown, Ed Bueler and Constantine Khroulev
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
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

#include <cmath>
#include <cstring>
#include <petscda.h>

#include "iceModel.hh"
#include "pism_signal.h"

IceModel::IceModel(IceGrid &g, NCConfigVariable &conf, NCConfigVariable &conf_overrides)
  : grid(g), config(conf), overrides(conf_overrides), iceFactory(grid.com,NULL, conf), ice(NULL) {
  PetscErrorCode ierr;

  if (utIsInit() == 0) {
    if (utInit(NULL) != 0) {
      PetscPrintf(grid.com, "PISM ERROR: UDUNITS initialization failed.\n");
      PetscEnd();
    }
  }

  mapping.init("mapping", grid.com, grid.rank);
  global_attributes.init("global_attributes", grid.com, grid.rank);

  pism_signal = 0;
  signal(SIGTERM, pism_signal_handler);
  signal(SIGUSR1, pism_signal_handler);
  signal(SIGUSR2, pism_signal_handler);

  doAdaptTimeStep = PETSC_TRUE;
  basal = NULL;
  top0ctx = PETSC_NULL;
  g2natural = PETSC_NULL;
  CFLviolcount = 0;

  surface = NULL;
  ocean = NULL;

  EC = NULL;

  ierr = setDefaults();  // lots of parameters and flags set here, including by reading from a config file
  if (ierr != 0) {
    verbPrintf(1,grid.com, "Error setting defaults.\n");
    PetscEnd();
  }

  // Special diagnostic viewers are off by default:
  view_diffusivity = false;
  view_nuH = false;
  view_log_nuH = false;

  // Do not save snapshots by default:
  save_snapshots = false;
  // Do not save time-series by default:
  save_ts = false;
  save_extra = false;

  dvoldt = gdHdtav = 0;
  total_surface_ice_flux = 0;
  total_basal_ice_flux = 0;
  total_sub_shelf_ice_flux = 0;

  have_ssa_velocities = 0;	// no SSA velocities at the start of the run

  allowAboveMelting = PETSC_FALSE;  // only IceCompModel ever sets it to true

  doColdIceMethods = PETSC_TRUE;  // FIXME: this way until IceEnthalpyModel is fully moved into IceModel

  // Default ice type:
  iceFactory.setType(ICE_PB);
}


IceModel::~IceModel() {

  deallocate_internal_objects();

  // write (and deallocate) time-series
  vector<DiagnosticTimeseries*>::iterator i;
  for (i = timeseries.begin(); i < timeseries.end(); ++i)
    delete (*i);

  delete ocean;
  delete surface;

  delete basal;
  delete EC;
  delete ice;

  utTerm(); // Clean up after UDUNITS
}


//! Allocate all IceModelVecs defined in IceModel.
/*!
  This procedure allocates the memory used to store model state, diagnostic and
  work vectors and sets metadata.

  Default values should not be set here; please use set_vars_from_options().

  All the memory allocated here is freed by IceModelVecs' destructors.
*/
PetscErrorCode IceModel::createVecs() {
  PetscErrorCode ierr;

  ierr = verbPrintf(3, grid.com,
		    "Allocating memory...\n"); CHKERRQ(ierr);

  // The following code creates (and documents -- to some extent) the
  // variables. The main (and only) principle here is using standard names from
  // the CF conventions; see
  // http://cf-pcmdi.llnl.gov/documents/cf-standard-names

  ierr =     u3.create(grid, "uvel", true); CHKERRQ(ierr);
  ierr =     u3.set_attrs("diagnostic", "horizontal velocity of ice in the X direction",
			  "m s-1", "land_ice_x_velocity"); CHKERRQ(ierr);
  ierr =     u3.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  u3.write_in_glaciological_units = true;
  ierr = variables.add(u3); CHKERRQ(ierr);

  ierr =     v3.create(grid, "vvel", true); CHKERRQ(ierr);
  ierr =     v3.set_attrs("diagnostic", "horizontal velocity of ice in the Y direction",
			  "m s-1", "land_ice_y_velocity"); CHKERRQ(ierr);
  ierr =     v3.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  v3.write_in_glaciological_units = true;
  ierr = variables.add(v3); CHKERRQ(ierr);

  ierr =     w3.create(grid, "wvel", false); CHKERRQ(ierr); // never diff'ed in hor dirs
  // PROPOSED standard name = land_ice_upward_velocity
  //   (compare "upward_air_velocity" and "upward_sea_water_velocity")
  ierr =     w3.set_attrs("diagnostic", "vertical velocity of ice",
			  "m s-1", ""); CHKERRQ(ierr);
  ierr =     w3.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  w3.write_in_glaciological_units = true;
  ierr = variables.add(w3); CHKERRQ(ierr);

  ierr = Sigma3.create(grid, "strainheat", false); CHKERRQ(ierr); // never diff'ed in hor dirs
  ierr = Sigma3.set_attrs("internal",
                          "rate of strain heating in ice (dissipation heating)",
	        	  "W m-3", ""); CHKERRQ(ierr);
  ierr = Sigma3.set_glaciological_units("mW m-3"); CHKERRQ(ierr);
  ierr = variables.add(Sigma3); CHKERRQ(ierr);

  // ice temperature
  ierr = T3.create(grid, "temp", true); CHKERRQ(ierr);
  ierr = T3.set_attrs("model_state","ice temperature",
		      "K", "land_ice_temperature"); CHKERRQ(ierr);
  ierr = T3.set_attr("valid_min", 0.0); CHKERRQ(ierr);
  ierr = variables.add(T3); CHKERRQ(ierr);

  // age of ice but only if age will be computed
  if (config.get_flag("do_age")) {
    ierr = tau3.create(grid, "age", true); CHKERRQ(ierr);
    // PROPOSED standard_name = land_ice_age
    ierr = tau3.set_attrs("model_state", "age of ice",
                          "s", ""); CHKERRQ(ierr);
    ierr = tau3.set_glaciological_units("years");
    tau3.write_in_glaciological_units = true;
    ierr = tau3.set_attr("valid_min", 0.0); CHKERRQ(ierr);
    ierr = variables.add(tau3); CHKERRQ(ierr);
  }

  // bedrock temperature
  ierr = Tb3.create(grid,"litho_temp", false); CHKERRQ(ierr);
  // PROPOSED standard_name = lithosphere_temperature
  ierr = Tb3.set_attrs("model_state", "lithosphere (bedrock) temperature",
		       "K", ""); CHKERRQ(ierr);
  ierr = Tb3.set_attr("valid_min", 0.0); CHKERRQ(ierr);
  ierr = variables.add(Tb3); CHKERRQ(ierr);

  // ice upper surface elevation
  ierr = vh.create(grid, "usurf", true); CHKERRQ(ierr);
  ierr = vh.set_attrs("diagnostic", "ice upper surface elevation",
		      "m", "surface_altitude"); CHKERRQ(ierr);
  ierr = variables.add(vh); CHKERRQ(ierr);

  // land ice thickness
  ierr = vH.create(grid, "thk", true); CHKERRQ(ierr);
  ierr = vH.set_attrs("model_state", "land ice thickness",
		      "m", "land_ice_thickness"); CHKERRQ(ierr);
  ierr = vH.set_attr("valid_min", 0.0); CHKERRQ(ierr);
  ierr = variables.add(vH); CHKERRQ(ierr);

  // bedrock surface elevation
  ierr = vbed.create(grid, "topg", true); CHKERRQ(ierr);
  ierr = vbed.set_attrs("model_state", "bedrock surface elevation",
			"m", "bedrock_altitude"); CHKERRQ(ierr);
  ierr = variables.add(vbed); CHKERRQ(ierr);

  // grounded_dragging_floating integer mask
  ierr = vMask.create(grid, "mask", true); CHKERRQ(ierr);
  ierr = vMask.set_attrs("model_state", "grounded_dragging_floating integer mask",
			 "", ""); CHKERRQ(ierr);
  vector<double> mask_values(6);
  mask_values[0] = MASK_ICE_FREE_BEDROCK;
  mask_values[1] = MASK_SHEET;
  mask_values[2] = MASK_DRAGGING_SHEET;
  mask_values[3] = MASK_FLOATING;
  mask_values[4] = MASK_ICE_FREE_OCEAN;
  mask_values[5] = MASK_OCEAN_AT_TIME_0;
  ierr = vMask.set_attr("flag_values", mask_values); CHKERRQ(ierr);
  ierr = vMask.set_attr("flag_meanings",
			"ice_free_bedrock sheet dragging_sheet floating ice_free_ocean ocean_at_time_zero"); CHKERRQ(ierr);
  vMask.output_data_type = NC_BYTE;
  ierr = variables.add(vMask); CHKERRQ(ierr);

  // upward geothermal flux at bedrock surface
  ierr = vGhf.create(grid, "bheatflx", false); CHKERRQ(ierr); // never differentiated
  // PROPOSED standard_name = lithosphere_upward_heat_flux
  ierr = vGhf.set_attrs("climate_steady", "upward geothermal flux at bedrock surface",
			"W m-2", ""); CHKERRQ(ierr);
  ierr = vGhf.set_glaciological_units("mW m-2");
  vGhf.time_independent = true;
  ierr = variables.add(vGhf); CHKERRQ(ierr);

  // u bar and v bar
  ierr = vubar.create(grid, "ubar", true); CHKERRQ(ierr);
  ierr = vubar.set_attrs("diagnostic", 
                         "vertical mean of horizontal ice velocity in the X direction",
			 "m s-1", "land_ice_vertical_mean_x_velocity"); CHKERRQ(ierr);
  ierr = vubar.set_glaciological_units("m year-1");
  vubar.write_in_glaciological_units = true;
  ierr = variables.add(vubar); CHKERRQ(ierr);

  ierr = vvbar.create(grid, "vbar", true); CHKERRQ(ierr);
  ierr = vvbar.set_attrs("diagnostic", 
                         "vertical mean of horizontal ice velocity in the Y direction",
			 "m s-1", "land_ice_vertical_mean_y_velocity"); CHKERRQ(ierr);
  ierr = vvbar.set_glaciological_units("m year-1");
  vvbar.write_in_glaciological_units = true;
  ierr = variables.add(vvbar); CHKERRQ(ierr);

  // basal velocities on standard grid
  ierr = vub.create(grid, "ub", true); CHKERRQ(ierr);
  ierr = vub.set_attrs("diagnostic", "basal ice velocity in the X direction",
		       "m s-1", "land_ice_basal_x_velocity"); CHKERRQ(ierr);
  ierr = vub.set_glaciological_units("m year-1");
  vub.write_in_glaciological_units = true;
  ierr = variables.add(vub); CHKERRQ(ierr);
  
  ierr = vvb.create(grid, "vb", true); CHKERRQ(ierr);
  ierr = vvb.set_attrs("diagnostic", "basal ice velocity in the Y direction",
		       "m s-1", "land_ice_basal_y_velocity"); CHKERRQ(ierr);
  ierr = vvb.set_glaciological_units("m year-1");
  vvb.write_in_glaciological_units = true;
  ierr = variables.add(vvb); CHKERRQ(ierr);

  // basal frictional heating on regular grid
  ierr = vRb.create(grid, "bfrict", true); CHKERRQ(ierr);
  // PROPOSED standard_name = land_ice_basal_frictional_heating
  ierr = vRb.set_attrs("diagnostic",
                       "basal frictional heating from ice sliding (= till dissipation)",
		       "W m-2", ""); CHKERRQ(ierr);
  ierr = vRb.set_glaciological_units("mW m-2");
  vRb.write_in_glaciological_units = true;
  ierr = vRb.set_attr("valid_min", 0.0); CHKERRQ(ierr);
  ierr = variables.add(vRb); CHKERRQ(ierr);

  // effective thickness of subglacial melt water
  ierr = vHmelt.create(grid, "bwat", true); CHKERRQ(ierr);
  ierr = vHmelt.set_attrs("model_state", "effective thickness of subglacial melt water",
			  "m", ""); CHKERRQ(ierr);
  // NB! Effective thickness of subglacial melt water *does* vary from 0 to hmelt_max meters only.
  ierr = vHmelt.set_attr("valid_min", 0.0); CHKERRQ(ierr);
  ierr = vHmelt.set_attr("valid_max", config.get("hmelt_max")); CHKERRQ(ierr);
  ierr = variables.add(vHmelt); CHKERRQ(ierr);

  // rate of change of ice thickness
  ierr = vdHdt.create(grid, "dHdt", true); CHKERRQ(ierr);
  ierr = vdHdt.set_attrs("diagnostic", "rate of change of ice thickness",
			 "m s-1", "tendency_of_land_ice_thickness"); CHKERRQ(ierr);
  ierr = vdHdt.set_glaciological_units("m year-1");
  vdHdt.write_in_glaciological_units = true;
  const PetscScalar  huge_dHdt = 1.0e6;      // million m a-1 is out-of-range
  ierr = vdHdt.set_attr("valid_min", -huge_dHdt / secpera); CHKERRQ(ierr);
  ierr = vdHdt.set_attr("valid_max", huge_dHdt / secpera); CHKERRQ(ierr);
  ierr = variables.add(vdHdt); CHKERRQ(ierr);

  // yield stress for basal till (plastic or pseudo-plastic model)
  ierr = vtauc.create(grid, "tauc", true); CHKERRQ(ierr);
  // PROPOSED standard_name = land_ice_basal_material_yield_stress
  ierr = vtauc.set_attrs("diagnostic", 
             "yield stress for basal till (plastic or pseudo-plastic model)",
	     "Pa", ""); CHKERRQ(ierr);
  ierr = variables.add(vtauc); CHKERRQ(ierr);

  // bedrock uplift rate
  ierr = vuplift.create(grid, "dbdt", true); CHKERRQ(ierr);
  ierr = vuplift.set_attrs("model_state", "bedrock uplift rate",
			   "m s-1", "tendency_of_bedrock_altitude"); CHKERRQ(ierr);
  ierr = vuplift.set_glaciological_units("m year-1");
  vuplift.write_in_glaciological_units = true;
  ierr = variables.add(vuplift); CHKERRQ(ierr);

  // basal melt rate
  ierr = vbasalMeltRate.create(grid, "bmelt", true); CHKERRQ(ierr);
  ierr = vbasalMeltRate.set_attrs("model_state",
                                  "ice basal melt rate in ice thickness per time",
				  "m s-1", "land_ice_basal_melt_rate"); CHKERRQ(ierr);
  ierr = vbasalMeltRate.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  vbasalMeltRate.write_in_glaciological_units = true;
  vbasalMeltRate.set_attr("comment", "positive basal melt rate corresponds to ice loss");
  ierr = variables.add(vbasalMeltRate); CHKERRQ(ierr);

  // friction angle for till under grounded ice sheet
  ierr = vtillphi.create(grid, "tillphi", false); // never differentiated
  // PROPOSED standard_name = land_ice_basal_material_friction_angle
  ierr = vtillphi.set_attrs("climate_steady", "friction angle for till under grounded ice sheet",
			    "degrees", ""); CHKERRQ(ierr);
  vtillphi.time_independent = true;
  ierr = variables.add(vtillphi); CHKERRQ(ierr);

  // longitude
  ierr = vLongitude.create(grid, "lon", false); CHKERRQ(ierr);
  ierr = vLongitude.set_attrs("mapping", "longitude", "degree_east", "longitude"); CHKERRQ(ierr);
  vLongitude.time_independent = true;
  ierr = variables.add(vLongitude); CHKERRQ(ierr);

  // latitude
  ierr = vLatitude.create(grid, "lat", false); CHKERRQ(ierr);
  ierr = vLatitude.set_attrs("mapping", "latitude", "degree_north", "latitude"); CHKERRQ(ierr);
  vLatitude.time_independent = true;
  ierr = variables.add(vLatitude); CHKERRQ(ierr);

  // u bar and v bar on staggered grid
  ierr = vuvbar[0].create(grid, "vuvbar[0]", true); CHKERRQ(ierr);
  ierr = vuvbar[0].set_attrs("internal", 
            "vertically averaged ice velocity, on staggered grid offset in X direction, from SIA, in the X direction",
	    "m s-1", ""); CHKERRQ(ierr);
  ierr = vuvbar[1].create(grid, "vuvbar[1]", true); CHKERRQ(ierr);
  ierr = vuvbar[1].set_attrs("internal", 
            "vertically averaged ice velocity, on staggered grid offset in Y direction, from SIA, in the Y direction",
	    "m s-1", ""); CHKERRQ(ierr);

  // initial guesses of SSA velocities
  ierr = vubarSSA.create(grid, "vubarSSA", true);
  ierr = vubarSSA.set_attrs("internal_restart", "SSA model ice velocity in the X direction",
                            "m s-1", ""); CHKERRQ(ierr);
  ierr = vubarSSA.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  ierr = variables.add(vubarSSA); CHKERRQ(ierr);

  ierr = vvbarSSA.create(grid, "vvbarSSA", true);
  ierr = vvbarSSA.set_attrs("internal_restart", "SSA model ice velocity in the Y direction",
                            "m s-1", ""); CHKERRQ(ierr);
  ierr = vvbarSSA.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  ierr = variables.add(vvbarSSA); CHKERRQ(ierr);

  // input fields:
  // mean annual net ice equivalent surface mass balance rate
  ierr = acab.create(grid, "acab", false); CHKERRQ(ierr);
  ierr = acab.set_attrs(
            "climate_state",
            "instantaneous ice-equivalent surface mass balance (accumulation/ablation) rate",
	    "m s-1",  // m *ice-equivalent* per second
	    "land_ice_surface_specific_mass_balance");  // CF standard_name
	    CHKERRQ(ierr);
  ierr = acab.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  acab.write_in_glaciological_units = true;
  acab.set_attr("comment", "positive values correspond to ice gain");
  ierr = variables.add(acab); CHKERRQ(ierr);

  // annual mean air temperature at "ice surface", at level below all firn
  //   processes (e.g. "10 m" or ice temperatures)
  ierr = artm.create(grid, "artm", false); CHKERRQ(ierr);
  ierr = artm.set_attrs(
            "climate_state",
            "time-dependent annual average ice temperature at ice surface but below firn processes",
            "K", 
            "");  // PROPOSED CF standard_name = land_ice_surface_temperature_below_firn
            CHKERRQ(ierr);
  ierr = variables.add(artm); CHKERRQ(ierr);

  // ice mass balance rate at the base of the ice shelf; sign convention for
  //   vshelfbasemass matches standard sign convention for basal melt rate of
  //   grounded ice
  ierr = shelfbmassflux.create(grid, "shelfbmassflux", false); CHKERRQ(ierr); // no ghosts; NO HOR. DIFF.!
  ierr = shelfbmassflux.set_attrs(
           "climate_state", "ice mass flux from ice shelf base (positive flux is loss from ice shelf)",
	   "m s-1", ""); CHKERRQ(ierr); 
  // PROPOSED standard name = ice_shelf_basal_specific_mass_balance
  // rescales from m/s to m/a when writing to NetCDF and std out:
  shelfbmassflux.write_in_glaciological_units = true;
  ierr = shelfbmassflux.set_glaciological_units("m year-1"); CHKERRQ(ierr);
  ierr = variables.add(shelfbmassflux); CHKERRQ(ierr);

  // ice boundary tempature at the base of the ice shelf
  ierr = shelfbtemp.create(grid, "shelfbtemp", false); CHKERRQ(ierr); // no ghosts; NO HOR. DIFF.!
  ierr = shelfbtemp.set_attrs(
           "climate_state", "absolute temperature at ice shelf base",
	   "K", ""); CHKERRQ(ierr);
  // PROPOSED standard name = ice_shelf_basal_temperature
  ierr = variables.add(shelfbtemp); CHKERRQ(ierr);

  return 0;
}


//! De-allocate internal objects.
/*! This includes Vecs that are not in an IceModelVec, SSA tools and the bed
  deformation model.
 */
PetscErrorCode IceModel::deallocate_internal_objects() {
  PetscErrorCode ierr;

  ierr = bedDefCleanup(); CHKERRQ(ierr);

  ierr = VecDestroy(g2); CHKERRQ(ierr);

  ierr = KSPDestroy(SSAKSP); CHKERRQ(ierr);
  ierr = MatDestroy(SSAStiffnessMatrix); CHKERRQ(ierr);
  ierr = VecDestroy(SSAX); CHKERRQ(ierr);
  ierr = VecDestroy(SSARHS); CHKERRQ(ierr);
  ierr = VecDestroy(SSAXLocal); CHKERRQ(ierr);
  ierr = VecScatterDestroy(SSAScatterGlobalToLocal); CHKERRQ(ierr);

  return 0;
}

void IceModel::setConstantNuHForSSA(PetscScalar nuH) {
  config.set_flag("use_constant_nuh_for_ssa", true);
  ssaStrengthExtend.set_notional_strength(nuH);
}


PetscErrorCode IceModel::setExecName(const char *my_executable_short_name) {
  executable_short_name = my_executable_short_name;
  return 0;
}

PetscErrorCode IceModel::step(bool do_mass_conserve,
			      bool do_temp,
			      bool do_age,
			      bool do_skip,
			      bool do_bed_deformation,
			      bool do_plastic_till) {
  PetscErrorCode ierr;

  ierr = additionalAtStartTimestep(); CHKERRQ(ierr);  // might set dt_force,maxdt_temporary

  // ask boundary models what the maximum time-step should be
  double apcc_dt;
  ierr = surface->max_timestep(grid.year, apcc_dt); CHKERRQ(ierr);
  apcc_dt *= secpera;
  if (apcc_dt > 0.0) {
    if (maxdt_temporary > 0)
      maxdt_temporary = PetscMin(apcc_dt, maxdt_temporary);
    else
      maxdt_temporary = apcc_dt;
  }

  double opcc_dt;
  ierr = ocean->max_timestep(grid.year, opcc_dt); CHKERRQ(ierr);
  opcc_dt *= secpera;
  if (opcc_dt > 0.0) {
    if (maxdt_temporary > 0)
      maxdt_temporary = PetscMin(opcc_dt, maxdt_temporary);
    else
      maxdt_temporary = opcc_dt;
  }

  // -extra_{times,file,vars} mechanism:
  double extras_dt;
  ierr = extras_max_timestep(grid.year, extras_dt); CHKERRQ(ierr);
  extras_dt *= secpera;
  if (extras_dt > 0.0) {
    if (maxdt_temporary > 0)
      maxdt_temporary = PetscMin(extras_dt, maxdt_temporary);
    else
      maxdt_temporary = extras_dt;
  }

  PetscLogEventBegin(beddefEVENT,0,0,0,0);

  // compute bed deformation, which only depends on current thickness and bed elevation
  if (do_bed_deformation) {
    ierr = bedDefStepIfNeeded(); CHKERRQ(ierr); // prints "b" or "$" as appropriate
  } else stdout_flags += " ";
    
  PetscLogEventEnd(beddefEVENT,0,0,0,0);

  // update basal till yield stress if appropriate; will modify and communicate mask
  if (do_plastic_till) {
    ierr = updateYieldStressUsingBasalWater();  CHKERRQ(ierr);
    stdout_flags += "y";
  } else stdout_flags += "$";


  // always do SIA velocity calculation; only update SSA and 
  //   only update velocities at depth if suggested by temp and age
  //   stability criterion; note *lots* of communication is avoided by 
  //   skipping SSA (and temp/age)
  bool updateAtDepth = (skipCountDown == 0);
  ierr = velocity(updateAtDepth); CHKERRQ(ierr);  // event logging in here
  stdout_flags += (updateAtDepth ? "v" : "V");
    
  // adapt time step using velocities and diffusivity, ..., just computed
  bool useCFLforTempAgeEqntoGetTimestep = (do_temp == PETSC_TRUE);
  ierr = determineTimeStep(useCFLforTempAgeEqntoGetTimestep); CHKERRQ(ierr);
  dtTempAge += dt;
  grid.year += dt / secpera;  // adopt it
  // IceModel::dt,dtTempAge,grid.year are now set correctly according to
  //    mass-continuity-eqn-diffusivity criteria, horizontal CFL criteria, and other 
  //    criteria from derived class additionalAtStartTimestep(), and from 
  //    "-skip" mechanism

  PetscLogEventBegin(tempEVENT,0,0,0,0);

  if (do_age) {
    ierr = ageStep(); CHKERRQ(ierr);
    stdout_flags += "a";
  } else {
    stdout_flags += "$";
  }

  bool tempStep = updateAtDepth && do_temp;
  if (tempStep) { // do the temperature step
    ierr = temperatureStep(); CHKERRQ(ierr);
    if (updateHmelt == PETSC_TRUE) {
      ierr = diffuseHmelt(); CHKERRQ(ierr);
    }
    stdout_flags += "t";
  } else {
    stdout_flags += "$";
  }

  dtTempAge = 0.0;

  PetscLogEventEnd(tempEVENT,0,0,0,0);

  ierr = ice_mass_bookkeeping(); CHKERRQ(ierr);

  PetscLogEventBegin(massbalEVENT,0,0,0,0);

  if (do_mass_conserve) {
    ierr = massContExplicitStep(); CHKERRQ(ierr); // update H
    ierr = updateSurfaceElevationAndMask(); CHKERRQ(ierr); // update h and mask
    if ((do_skip == PETSC_TRUE) && (skipCountDown > 0))
      skipCountDown--;
    stdout_flags += "h";
  } else {
    stdout_flags += "$";
    // if do_mass_conserve is false, then ice thickness does not change and
    // dH/dt = 0:
    ierr = vdHdt.set(0.0); CHKERRQ(ierr);
  }

  PetscLogEventEnd(massbalEVENT,0,0,0,0);
    
  ierr = additionalAtEndTimestep(); CHKERRQ(ierr);

  // end the flag line
  char tempstr[5];  snprintf(tempstr,5," %c", adaptReasonFlag);
  stdout_flags += tempstr;

  //  ierr = variables.check_for_nan(); CHKERRQ(ierr);

  return 0;
}

//! Do the time-stepping for an evolution run.
/*! 
This procedure is the main time-stepping loop.  The following actions are taken on each pass 
through the loop:
\li the yield stress for the plastic till model is updated (if appropriate)
\li the positive degree day model is invoked to compute the surface mass balance (if appropriate)
\li a step of the bed deformation model is taken (if appropriate)
\li the velocity field is updated; in some cases the whole three-dimensional field is updated 
    and in some cases just the vertically-averaged horizontal velocity is updated; see velocity()
\li the time step is determined according to a variety of stability criteria; 
    see determineTimeStep()
\li the temperature field is updated according to the conservation of energy model based 
    (especially) on the new velocity field; see temperatureAgeStep()
\li the thickness of the ice is updated according to the mass conservation model; see
    massContExplicitStep()
\li there is various reporting to the user on the current state; see summary() and updateViewers()

Note that at the beginning and ends of each pass throught the loop there is a chance for 
derived classes to do extra work.  See additionalAtStartTimestep() and additionalAtEndTimestep().
 */
PetscErrorCode IceModel::run() {
  PetscErrorCode  ierr;

  bool do_mass_conserve = config.get_flag("do_mass_conserve"),
    do_temp = config.get_flag("do_temp"),
    do_age = config.get_flag("do_age"),
    do_skip = config.get_flag("do_skip"),
    do_bed_deformation = config.get_flag("do_bed_deformation"),
    do_plastic_till = config.get_flag("do_plastic_till");

#if PETSC_VERSION_MAJOR >= 3
# define PismLogEventRegister(name,cookie,event) PetscLogEventRegister((name),(cookie),(event))
#else
# define PismLogEventRegister(name,cookie,event) PetscLogEventRegister((event),(name),(cookie))
#endif
PismLogEventRegister("sia velocity", 0,&siaEVENT);
PismLogEventRegister("ssa velocity", 0,&ssaEVENT);
PismLogEventRegister("misc vel calc",0,&velmiscEVENT);
PismLogEventRegister("bed deform",   0,&beddefEVENT);
PismLogEventRegister("mass bal calc",0,&massbalEVENT);
PismLogEventRegister("temp age calc",0,&tempEVENT);

  // do a one-step diagnostic run:

  ierr = verbPrintf(3,grid.com,
      "  doing preliminary step to fill diagnostic quantities ..."); CHKERRQ(ierr);

  // set verbosity to 1 to suppress reporting
  PetscInt tmp_verbosity = getVerbosityLevel(); 
  ierr = setVerbosityLevel(1); CHKERRQ(ierr);

  dt_force = -1.0;
  maxdt_temporary = -1.0;
  skipCountDown = 0;
  dtTempAge = 0.0;
  dt = 0.0;
  PetscReal end_year = grid.end_year;
  grid.end_year = grid.start_year + 1; // all what matters is that it is
				       // greater than start_year

  ierr = step(do_mass_conserve, do_temp, do_age,
	      do_skip, do_bed_deformation, do_plastic_till); CHKERRQ(ierr);

  // print verbose messages according to user-set verbosity
  if (tmp_verbosity > 2) {
    ierr = PetscPrintf(grid.com,
      " done; reached time %.4f a\n", grid.year); CHKERRQ(ierr);
    ierr = PetscPrintf(grid.com,
      "  re-setting model state as initialized ...\n"); CHKERRQ(ierr);
  }

  // re-initialize the model:
  global_attributes.set_string("history", "");
  grid.year = grid.start_year;
  grid.end_year = end_year;
  ierr = model_state_setup(); CHKERRQ(ierr);

  // restore verbosity:
  ierr = setVerbosityLevel(tmp_verbosity); CHKERRQ(ierr);

  // Write snapshots and time-series at the beginning of the run.
  ierr = write_snapshot(); CHKERRQ(ierr);
  ierr = write_timeseries(); CHKERRQ(ierr);
  ierr = write_extras(); CHKERRQ(ierr);

  stdout_flags.erase(); // clear it out

  ierr = summaryPrintLine(PETSC_TRUE,do_temp, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0); CHKERRQ(ierr);
  adaptReasonFlag = '$'; // no reason for no timestep
  skipCountDown = 0;
  dtTempAge = 0.0;
  maxdt_temporary = dt = dt_force = 0.0;
  dt_from_diffus = dt_from_cfl = 0.0;
  CFLmaxdt = CFLmaxdt2D = 0.0;
  gDmax = dvoldt = gdHdtav = 0;
  total_surface_ice_flux = 0;
  total_basal_ice_flux = 0;
  total_sub_shelf_ice_flux = 0;

  gmaxu = gmaxv = gmaxw = -1;

  ierr = summary(do_temp,reportPATemps); CHKERRQ(ierr);  // report starting state

  // main loop for time evolution
  for (PetscScalar year = grid.start_year; year < grid.end_year; year += dt/secpera) {
    
    stdout_flags.erase();  // clear it out
    dt_force = -1.0;
    maxdt_temporary = -1.0;

    ierr = step(do_mass_conserve, do_temp, do_age,
		do_skip, do_bed_deformation, do_plastic_till); CHKERRQ(ierr);

    // report a summary for major steps or the last one
    bool updateAtDepth = (skipCountDown == 0);
    bool tempAgeStep = ( updateAtDepth && ((do_temp) || (do_age)) );

    const bool show_step = tempAgeStep || (adaptReasonFlag == 'e');
    ierr = summary(show_step,reportPATemps); CHKERRQ(ierr);

    // writing these fields here ensures that we do it after the last time-step
    ierr = write_snapshot(); CHKERRQ(ierr);
    ierr = write_timeseries(); CHKERRQ(ierr);
    ierr = write_extras(); CHKERRQ(ierr);

    ierr = update_viewers(); CHKERRQ(ierr);

    if (endOfTimeStepHook() != 0) break;
  } // end of the time-stepping loop

  return 0;
}


//! Calls the necessary routines to do a diagnostic calculation of velocity.
/*! 
This important routine can be replaced by derived classes; it is \c virtual.

This procedure has no loop but the following actions are taken:
\li the yield stress for the plastic till model is updated (if appropriate)
\li the velocity field is updated; in some cases the whole three-dimensional field is updated 
    and in some cases just the vertically-averaged horizontal velocity is updated; see velocity()
\li there is various reporting to the user on the current state; see summary() and updateViewers()
 */
PetscErrorCode IceModel::diagnosticRun() {
  PetscErrorCode  ierr;

  bool do_plastic_till = config.get_flag("do_plastic_till");

  // print out some stats about input state
  ierr = summaryPrintLine(PETSC_TRUE,PETSC_TRUE, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
           CHKERRQ(ierr);
  adaptReasonFlag = ' '; // no reason for no timestep
  skipCountDown = 0;
  dt = 0.0;

  // update basal till yield stress if appropriate; will modify and communicate mask
  if (do_plastic_till) {
    ierr = updateYieldStressUsingBasalWater();  CHKERRQ(ierr);
  }

  ierr = velocity(true); CHKERRQ(ierr);  // compute velocities (at depth)

  ierr = summary(true,true); CHKERRQ(ierr);
  
  // update viewers and pause for a chance to view
  ierr = update_viewers(); CHKERRQ(ierr);
  bool flag;
  PetscInt pause_time = 0;
  ierr = PISMOptionsInt("-pause", "Pause after the run, seconds",
			pause_time, flag); CHKERRQ(ierr);
  if (pause_time > 0) {
    ierr = verbPrintf(2,grid.com,"pausing for %d secs ...\n",pause_time); CHKERRQ(ierr);
    ierr = PetscSleep(pause_time); CHKERRQ(ierr);
  }
  return 0;
}

//! Manage the initialization of the IceModel object.
/*!
The IceModel initialization sequence is this:
  
   1) Initialize the computational grid.

   2) Process the options.

   3) Memory allocation.

   4) Initialize IceFlowLaw and (possibly) other physics.

   5) Initialize PDD and forcing.

   6) Fill the model state variables (from a PISM output file, from a
   bootstrapping file using some modeling choices or using formulas). Regrid.

   8) Report grid parameters.

   9) Allocate internal objects: SSA tools and work vectors. Some tasks in the
   next (tenth) item (bed deformation setup, for example) might need this.

   10) Miscellaneous stuff: set up the bed deformation model, initialize the
   basal till model, initialize snapshots.

Please see the documenting comments of the functions called below to find 
explanations of their intended uses.

The following flow-chart illustrates the process.

\dotfile initialization-sequence.dot IceModel initialization sequence
 */
PetscErrorCode IceModel::init() {
  PetscErrorCode ierr;

  ierr = PetscOptionsBegin(grid.com, "", "PISM options", ""); CHKERRQ(ierr);

  // Build PISM with PISM_WAIT_FOR_GDB defined and run with -wait_for_gdb to
  // make it wait for a connection.
#ifdef PISM_WAIT_FOR_GDB
  bool wait_for_gdb = false;
  ierr = PISMOptionsIsSet("-wait_for_gdb", wait_for_gdb); CHKERRQ(ierr);
  if (wait_for_gdb) {
    ierr = pism_wait_for_gdb(grid.com, 0); CHKERRQ(ierr);
  }
#endif

  // 1) Initialize the computational grid:
  ierr = grid_setup(); CHKERRQ(ierr);

  // 2) Process the options:
  ierr = setFromOptions(); CHKERRQ(ierr);

  // 3) Memory allocation:
  ierr = createVecs(); CHKERRQ(ierr);

  // 4) Initialize the IceFlowLaw and (possibly) other physics.
  ierr = init_physics(); CHKERRQ(ierr);

  // 5) Initialize atmosphere and ocean couplers:
  ierr = init_couplers(); CHKERRQ(ierr);

  // 6) Fill the model state variables (from a PISM output file, from a
  // bootstrapping file using some modeling choices or using formulas).
  // Calls IceModel::regrid()
  ierr = model_state_setup(); CHKERRQ(ierr);

  // 8) Report grid parameters:
  ierr = report_grid_parameters(); CHKERRQ(ierr);

  // 9) Allocate SSA tools and work vectors:
  ierr = allocate_internal_objects(); CHKERRQ(ierr);

  // 10) Miscellaneous stuff: set up the bed deformation model, initialize the
  // basal till model, initialize snapshots. This has to happen *after*
  // regridding.
  ierr = misc_setup();

  ierr = PetscOptionsEnd(); CHKERRQ(ierr);

  return 0; 
}
