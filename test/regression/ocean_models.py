"""
Tests of PISM's ocean models and modifiers.
"""

import PISM
import sys, os
import numpy as np
from unittest import TestCase
import netCDF4

config = PISM.Context().config
# change the default melange back pressure fraction from 0 to 1. The default of zero makes
# it hard to test the modifier that scales the value.

config.set_double("ocean.constant.melange_back_pressure_fraction", 1.0)
log = PISM.Context().log
log.set_threshold(1)
options = PISM.PETSc.Options()

def create_dummy_forcing_file(filename, variable_name, units, value):
    f = netCDF4.Dataset(filename, "w")
    f.createDimension("time", 1)
    t = f.createVariable("time", "d", ("time",))
    t.units = "seconds"
    delta_T = f.createVariable(variable_name, "d", ("time",))
    delta_T.units = units
    t[0] = 0.0
    delta_T[0] = value
    f.close()

def dummy_grid():
    "Create a dummy grid"
    ctx = PISM.Context()
    params = PISM.GridParameters(ctx.config)
    params.ownership_ranges_from_options(ctx.size)
    return PISM.IceGrid(ctx.ctx, params)

def create_given_input_file(filename, grid, temperature, mass_flux):
    PISM.util.prepare_output(filename)

    T = PISM.IceModelVec2S(grid, "shelfbtemp", PISM.WITHOUT_GHOSTS)
    T.set_attrs("climate", "shelf base temperature", "Kelvin", "")
    T.set(temperature)
    T.write(filename)

    M = PISM.IceModelVec2S(grid, "shelfbmassflux", PISM.WITHOUT_GHOSTS)
    M.set_attrs("climate", "shelf base mass flux", "kg m-2 s-1", "")
    M.set(mass_flux)
    M.write(filename)

def check(vec, value):
    "Check if values of vec are almost equal to value."
    grid = vec.grid()
    with PISM.vec.Access(nocomm=[vec]):
        for (i, j) in grid.points():
            np.testing.assert_almost_equal(vec[i, j], value)

def check_difference(A, B, value):
    "Check if the difference between A and B is almost equal to value."
    grid = A.grid()
    with PISM.vec.Access(nocomm=[A, B]):
        for (i, j) in grid.points():
            np.testing.assert_almost_equal(A[i, j] - B[i, j], value)

def check_ratio(A, B, value):
    "Check if the difference between A and B is almost equal to value."
    grid = A.grid()
    with PISM.vec.Access(nocomm=[A, B]):
        for (i, j) in grid.points():
            if B[i, j] != 0:
                np.testing.assert_almost_equal(A[i, j] / B[i, j], value)

def check_model(model, T, SMB, SL, MBP):
    check(model.shelf_base_temperature(), T)
    check(model.shelf_base_mass_flux(), SMB)
    np.testing.assert_almost_equal(model.sea_level_elevation(), SL)
    check(model.melange_back_pressure_fraction(), MBP)

def check_modifier(model, modifier, dT, dSMB, dSL, dMBP):
    np.testing.assert_almost_equal(modifier.sea_level_elevation() - model.sea_level_elevation(),
                                   dSL)

    check_difference(modifier.shelf_base_temperature(),
                     model.shelf_base_temperature(),
                     dT)

    check_difference(modifier.shelf_base_mass_flux(),
                     model.shelf_base_mass_flux(),
                     dSMB)

    check_difference(modifier.melange_back_pressure_fraction(),
                     model.melange_back_pressure_fraction(),
                     dMBP)

def ocean_constant(grid, depth=1000.0):
    try:
        grid.variables.get_2d_scalar("thk")
    except:
        ice_thickness = PISM.model.createIceThicknessVec(grid)
        ice_thickness.set(depth)
        grid.variables().add(ice_thickness)

    return PISM.OceanConstant(grid)

def constant_test():
    "ocean::Constant"

    depth = 1000.0                  # meters

    # compute mass flux
    melt_rate   = config.get_double("ocean.constant.melt_rate", "m second-1")
    ice_density = config.get_double("constants.ice.density")
    mass_flux   = melt_rate * ice_density

    # compute pressure melting temperature
    T0          = config.get_double("constants.fresh_water.melting_point_temperature")
    beta_CC     = config.get_double("constants.ice.beta_Clausius_Clapeyron")
    g           = config.get_double("constants.standard_gravity")

    pressure  = ice_density * g * depth
    T_melting = T0 - beta_CC * pressure

    melange_back_pressure = 1.0

    sea_level = 0.0

    grid = dummy_grid()
    model = ocean_constant(grid, depth)

    model.init()
    model.update(0, 1)

    check_model(model, T_melting, mass_flux, sea_level, melange_back_pressure)

def pik_test():
    "ocean::PIK"
    grid = dummy_grid()

    depth = 1000.0                  # meters

    # compute pressure melting temperature
    ice_density = config.get_double("constants.ice.density")
    T0          = config.get_double("constants.fresh_water.melting_point_temperature")
    beta_CC     = config.get_double("constants.ice.beta_Clausius_Clapeyron")
    g           = config.get_double("constants.standard_gravity")

    pressure  = ice_density * g * depth
    T_melting = T0 - beta_CC * pressure

    melange_back_pressure = 0.0

    mass_flux = 5.36591610659e-06 # stored mass flux value returned by the model

    sea_level = 0.0

    # create the model
    ice_thickness = PISM.model.createIceThicknessVec(grid)
    ice_thickness.set(depth)
    grid.variables().add(ice_thickness)

    model = PISM.OceanPIK(grid)

    model.init()
    model.update(0, 1)

    check_model(model, T_melting, mass_flux, sea_level, melange_back_pressure)


class GivenTest(TestCase):
    "Test the ocean::Given class"

    def setUp(self):
        self.sea_level = 0.0

        self.grid = dummy_grid()
        self.filename = "given_input.nc"

        self.temperature           = 263.0
        self.mass_flux             = 3e-3
        self.melange_back_pressure = 0.0

        create_given_input_file(self.filename, self.grid, self.temperature, self.mass_flux)

        options.setValue("-ocean_given_file", self.filename)

    def runTest(self):
        "ocean::Given"

        model = PISM.OceanGiven(self.grid)
        model.init()
        model.update(0, 1)

        check_model(model, self.temperature, self.mass_flux, self.sea_level, self.melange_back_pressure)

    def tearDown(self):
        os.remove(self.filename)

class GivenTHTest(TestCase):
    def setUp(self):

        self.sea_level = 0.0

        depth                      = 1000.0
        salinity                   = 35.0
        potential_temperature      = 270.0
        self.melange_back_pressure = 0.0
        self.temperature           = 270.17909999999995
        self.mass_flux             = -6.489250000000001e-05

        self.grid = dummy_grid()

        ice_thickness = PISM.model.createIceThicknessVec(self.grid)
        ice_thickness.set(depth)
        self.grid.variables().add(ice_thickness)

        filename = "given_th_input.nc"
        self.filename = filename

        PISM.util.prepare_output(filename)

        Th = PISM.IceModelVec2S(self.grid, "theta_ocean", PISM.WITHOUT_GHOSTS)
        Th.set_attrs("climate", "potential temperature", "Kelvin", "")
        Th.set(potential_temperature)
        Th.write(filename)

        S = PISM.IceModelVec2S(self.grid, "salinity_ocean", PISM.WITHOUT_GHOSTS)
        S.set_attrs("climate", "ocean salinity", "g/kg", "")
        S.set(salinity)
        S.write(filename)

        options.setValue("-ocean_th_file", self.filename)

    def runTest(self):
        "ocean::GivenTH"

        model = PISM.OceanGivenTH(self.grid)
        model.init()
        model.update(0, 1)

        check_model(model, self.temperature, self.mass_flux, self.sea_level, self.melange_back_pressure)

    def tearDown(self):
        os.remove(self.filename)

class DeltaT(TestCase):
    def setUp(self):
        self.filename = "delta_T_input.nc"
        self.grid = dummy_grid()
        self.model = ocean_constant(self.grid)
        self.dT = -5.0

        create_dummy_forcing_file(self.filename, "delta_T", "Kelvin", self.dT)

    def runTest(self):
        "ocean::Delta_T"

        modifier = PISM.OceanDeltaT(self.grid, self.model)

        options.setValue("-ocean_delta_T_file", self.filename)

        modifier.init()
        modifier.update(0, 1)

        check_modifier(self.model, modifier, self.dT, 0.0, 0.0, 0.0)

    def tearDown(self):
        os.remove(self.filename)

class DeltaSL(TestCase):
    def setUp(self):
        self.filename = "delta_SL_input.nc"
        self.grid = dummy_grid()
        self.model = ocean_constant(self.grid)
        self.dSL = -5.0

        create_dummy_forcing_file(self.filename, "delta_SL", "meters", self.dSL)

    def runTest(self):
        "ocean::Delta_SL"

        modifier = PISM.OceanDeltaSL(self.grid, self.model)

        options.setValue("-ocean_delta_SL_file", self.filename)

        modifier.init()
        modifier.update(0, 1)

        check_modifier(self.model, modifier, 0.0, 0.0, self.dSL, 0.0)

    def tearDown(self):
        os.remove(self.filename)

class DeltaSMB(TestCase):
    def setUp(self):
        self.filename = "delta_SMB_input.nc"
        self.grid = dummy_grid()
        self.model = ocean_constant(self.grid)
        self.dSMB = -5.0

        create_dummy_forcing_file(self.filename, "delta_mass_flux", "kg m-2 s-1", self.dSMB)

    def runTest(self):
        "ocean::Delta_SMB"

        modifier = PISM.OceanDeltaSMB(self.grid, self.model)

        options.setValue("-ocean_delta_mass_flux_file", self.filename)

        modifier.init()
        modifier.update(0, 1)

        check_modifier(self.model, modifier, 0.0, self.dSMB, 0.0, 0.0)

    def tearDown(self):
        os.remove(self.filename)

class FracMBP(TestCase):
    def setUp(self):
        self.filename = "frac_MBP_input.nc"
        self.grid = dummy_grid()
        self.model = ocean_constant(self.grid)
        self.dMBP = 0.5

        create_dummy_forcing_file(self.filename, "frac_MBP", "1", self.dMBP)

    def runTest(self):
        "ocean::Frac_MBP"

        modifier = PISM.OceanFracMBP(self.grid, self.model)

        options.setValue("-ocean_frac_MBP_file", self.filename)

        modifier.init()
        modifier.update(0, 1)

        model = self.model

        np.testing.assert_almost_equal(modifier.sea_level_elevation(),
                                       model.sea_level_elevation())

        check_difference(modifier.shelf_base_temperature(),
                         model.shelf_base_temperature(),
                         0.0)

        check_difference(modifier.shelf_base_mass_flux(),
                         model.shelf_base_mass_flux(),
                         0.0)

        check_ratio(modifier.melange_back_pressure_fraction(),
                    model.melange_back_pressure_fraction(),
                    self.dMBP)

    def tearDown(self):
        os.remove(self.filename)

class FracSMB(TestCase):
    def setUp(self):
        self.filename = "frac_SMB_input.nc"
        self.grid = dummy_grid()
        self.model = ocean_constant(self.grid)
        self.dSMB = 0.5

        create_dummy_forcing_file(self.filename, "frac_mass_flux", "1", self.dSMB)

    def runTest(self):
        "ocean::Frac_SMB"

        modifier = PISM.OceanFracSMB(self.grid, self.model)

        options.setValue("-ocean_frac_mass_flux_file", self.filename)

        modifier.init()
        modifier.update(0, 1)

        model = self.model

        np.testing.assert_almost_equal(modifier.sea_level_elevation(),
                                       model.sea_level_elevation())

        check_difference(modifier.shelf_base_temperature(),
                         model.shelf_base_temperature(),
                         0.0)

        check_ratio(modifier.shelf_base_mass_flux(),
                    model.shelf_base_mass_flux(),
                    self.dSMB)

        check_difference(modifier.melange_back_pressure_fraction(),
                         model.melange_back_pressure_fraction(),
                         0.0)

    def tearDown(self):
        os.remove(self.filename)


if __name__ == "__main__":
    pass

# add_modifier<Cache>("cache");
