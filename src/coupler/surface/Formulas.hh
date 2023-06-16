/* Copyright (C) 2014, 2015, 2016, 2017, 2018, 2019, 2021 PISM Authors
 *
 * This file is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * PISM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PISM; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _PSFORMULAS_H_
#define _PSFORMULAS_H_

#include "pism/coupler/SurfaceModel.hh"

namespace pism {
namespace surface {

/** Base class for surface models that compute climate inputs using
 * formulas.
 *
 * Used by EISMINTII and Verification. 
 */
class PSFormulas : public SurfaceModel {
public:
  PSFormulas(std::shared_ptr<const Grid> g);
  ~PSFormulas() = default;
protected:

  virtual void define_model_state_impl(const File &output) const;
  virtual void write_model_state_impl(const File &output) const;

  const array::Scalar& mass_flux_impl() const;
  const array::Scalar& temperature_impl() const;

  const array::Scalar& accumulation_impl() const;
  const array::Scalar& melt_impl() const;
  const array::Scalar& runoff_impl() const;

protected:
  array::Scalar::Ptr m_mass_flux;
  array::Scalar::Ptr m_temperature;
};


} // end of namespace surface
} // end of namespace pism

#endif /* _PSFORMULAS_H_ */
