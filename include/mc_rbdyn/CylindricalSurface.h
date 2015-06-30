#pragma once

#include <mc_rbdyn/Surface.h>

namespace mc_rbdyn
{

struct Robot;

struct CylindricalSurfaceImpl;

struct CylindricalSurface : public Surface
{
  CylindricalSurface(const std::string & name, const std::string & bodyName, const sva::PTransformd & X_b_s, const std::string & materialName, const double & radius, const double & width);

  ~CylindricalSurface();

  virtual void computePoints();

  const double & radius() const;

  const double& width() const;

  void width(const double & width);

  virtual std::shared_ptr<Surface> copy() const;

  virtual std::string type() const override;
private:
  std::unique_ptr<CylindricalSurfaceImpl> impl;
};

}