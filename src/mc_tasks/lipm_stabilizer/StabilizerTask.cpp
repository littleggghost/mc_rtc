/*
 * Copyright 2015-2019 CNRS-UM LIRMM, CNRS-AIST JRL
 *
 * This file is inspired by Stephane's Caron original implementation as part of
 * lipm_walking_controller <https://github.com/stephane-caron/lipm_walking_controller>
 */

#include <mc_filter/utils/clamp.h>
#include <mc_rbdyn/constants.h>
#include <mc_rbdyn/rpy_utils.h>
#include <mc_rtc/gui.h>
#include <mc_rtc/gui/plot.h>
#include <mc_tasks/MetaTaskLoader.h>
#include <mc_tasks/lipm_stabilizer/StabilizerTask.h>

#include <chrono>

namespace mc_tasks
{
namespace lipm_stabilizer
{

using internal::Contact;
using ::mc_filter::utils::clamp;
using ::mc_filter::utils::clampInPlaceAndWarn;
namespace constants = ::mc_rbdyn::constants;

// Repeat static constexpr declarations
// Fixes https://github.com/stephane-caron/lipm_walking_controller/issues/21
// See also https://stackoverflow.com/q/8016780
constexpr double StabilizerTask::MAX_AVERAGE_DCM_ERROR;
constexpr double StabilizerTask::MAX_COM_ADMITTANCE;
constexpr double StabilizerTask::MAX_COP_ADMITTANCE;
constexpr double StabilizerTask::MAX_DCM_D_GAIN;
constexpr double StabilizerTask::MAX_DCM_I_GAIN;
constexpr double StabilizerTask::MAX_DCM_P_GAIN;
constexpr double StabilizerTask::MAX_DFZ_ADMITTANCE;
constexpr double StabilizerTask::MAX_DFZ_DAMPING;
constexpr double StabilizerTask::MAX_FDC_RX_VEL;
constexpr double StabilizerTask::MAX_FDC_RY_VEL;
constexpr double StabilizerTask::MAX_FDC_RZ_VEL;
constexpr double StabilizerTask::MIN_DS_PRESSURE;
constexpr double StabilizerTask::MIN_NET_TOTAL_FORCE_ZMP;

namespace
{
inline Eigen::Vector2d vecFromError(const Eigen::Vector3d & error)
{
  double x = -std::round(error.x() * 1000.);
  double y = -std::round(error.y() * 1000.);
  return Eigen::Vector2d{x, y};
}

} // namespace

StabilizerTask::StabilizerTask(const mc_rbdyn::Robots & robots,
                               const mc_rbdyn::Robots & realRobots,
                               unsigned int robotIndex,
                               const std::string & leftSurface,
                               const std::string & rightSurface,
                               const std::string & torsoBodyName,
                               double dt)
: robots_(robots), realRobots_(realRobots), robotIndex_(robotIndex), dcmIntegrator_(dt, /* timeConstant = */ 15.),
  dcmDerivator_(dt, /* timeConstant = */ 1.), dt_(dt), mass_(robots.robot(robotIndex).mass())
{
  type_ = "lipm_stabilizer";
  name_ = type_ + "_" + robots.robot(robotIndex).name();

  comTask.reset(new mc_tasks::CoMTask(robots, robotIndex_));
  auto leftCoP = std::make_shared<mc_tasks::force::CoPTask>(leftSurface, robots, robotIndex_);
  auto rightCoP = std::make_shared<mc_tasks::force::CoPTask>(rightSurface, robots, robotIndex_);
  footTasks[ContactState::Left] = leftCoP;
  footTasks[ContactState::Right] = rightCoP;

  std::string pelvisBodyName = robot().mb().body(0).name();
  pelvisTask = std::make_shared<mc_tasks::OrientationTask>(pelvisBodyName, robots_, robotIndex_);
  torsoTask = std::make_shared<mc_tasks::OrientationTask>(torsoBodyName, robots_, robotIndex_);

  // Rename the tasks managed by the stabilizer
  // Doing so helps making the logs more consistent, and having a fixed name
  // allows for predifined custom plots in the log ui.
  const auto n = name_ + "_Tasks";
  comTask->name(n + "_com");
  leftCoP->name(n + "_cop_left");
  rightCoP->name(n + "_cop_right");
  pelvisTask->name(n + "_pelvis");
  torsoTask->name(n + "_torso");

  configure(robot().module().defaultLIPMStabilizerConfiguration());
}

void StabilizerTask::reset()
{
  t_ = 0;
  configure(robot().module().defaultLIPMStabilizerConfiguration());
  comTask->reset();
  comTarget_ = comTask->com();

  for(auto footTask : footTasks)
  {
    footTask.second->reset();
    footTask.second->maxAngularVel({MAX_FDC_RX_VEL, MAX_FDC_RY_VEL, MAX_FDC_RZ_VEL});
  }

  pelvisTask->reset();
  torsoTask->reset();

  Eigen::Vector3d staticForce = mass_ * constants::gravity;

  dcmAverageError_ = Eigen::Vector3d::Zero();
  dcmError_ = Eigen::Vector3d::Zero();
  dcmVelError_ = Eigen::Vector3d::Zero();
  dfzForceError_ = 0.;
  dfzHeightError_ = 0.;
  distribWrench_ = {comTarget_.cross(staticForce), staticForce};
  vdcHeightError_ = 0.;
  zmpError_ = Eigen::Vector3d::Zero();

  dcmDerivator_.reset(Eigen::Vector3d::Zero());
  dcmIntegrator_.saturation(MAX_AVERAGE_DCM_ERROR);
  dcmIntegrator_.reset(Eigen::Vector3d::Zero());

  omega_ = std::sqrt(constants::gravity.z() / robot().com().z());
}

void StabilizerTask::dimWeight(const Eigen::VectorXd & /* dim */)
{
  LOG_ERROR_AND_THROW(std::runtime_error, "dimWeight not implemented for task " << type_);
}

Eigen::VectorXd StabilizerTask::dimWeight() const
{
  LOG_ERROR_AND_THROW(std::runtime_error, "dimWeight not implemented for task " << type_);
}

void StabilizerTask::selectActiveJoints(mc_solver::QPSolver & /* solver */,
                                        const std::vector<std::string> & /* activeJointsName */,
                                        const std::map<std::string, std::vector<std::array<int, 2>>> & /* activeDofs */)
{
  LOG_ERROR_AND_THROW(std::runtime_error, "Task " << name_
                                                  << " does not implement selectActiveJoints. Please configure it "
                                                     "through the stabilizer configuration instead");
}

void StabilizerTask::selectUnactiveJoints(
    mc_solver::QPSolver & /* solver */,
    const std::vector<std::string> & /* unactiveJointsName */,
    const std::map<std::string, std::vector<std::array<int, 2>>> & /* unactiveDofs */)
{
  LOG_ERROR_AND_THROW(std::runtime_error, "Task " << name_
                                                  << " does not implement selectUnactiveJoints. Please configure it "
                                                     "through the stabilizer configuration instead.");
}

void StabilizerTask::resetJointsSelector(mc_solver::QPSolver & /* solver */)
{
  LOG_ERROR_AND_THROW(std::runtime_error, "Task " << name_
                                                  << " does not implement resetJointsSelector. Please configure it "
                                                     "through the stabilizer configuration instead.");
}

Eigen::VectorXd StabilizerTask::eval() const
{
  Eigen::VectorXd res(3 + 3 * contactTasks.size());
  res.head(3) = comTask->eval();
  int i = 0;
  for(const auto & task : contactTasks)
  {
    res.segment(3 + 3 * i++, 3) = task->eval();
  }
  return res;
}

Eigen::VectorXd StabilizerTask::speed() const
{
  Eigen::VectorXd res(3 + 3 * contactTasks.size());
  res.head(3) = comTask->eval();
  int i = 0;
  for(const auto & task : contactTasks)
  {
    res.segment(3 + 3 * i++, 3) = task->speed();
  }
  return res;
}

void StabilizerTask::addToSolver(mc_solver::QPSolver & solver)
{
  // Feet tasks are added in update() instead, add all other tasks now
  MetaTask::addToSolver(*comTask, solver);
  MetaTask::addToSolver(*pelvisTask, solver);
  MetaTask::addToSolver(*torsoTask, solver);
}

void StabilizerTask::removeFromSolver(mc_solver::QPSolver & solver)
{
  MetaTask::removeFromSolver(*comTask, solver);
  MetaTask::removeFromSolver(*pelvisTask, solver);
  MetaTask::removeFromSolver(*torsoTask, solver);
  for(const auto footTask : contactTasks)
  {
    MetaTask::removeFromSolver(*footTask, solver);
  }
}

void StabilizerTask::updateContacts(mc_solver::QPSolver & solver)
{
  if(!addContacts_.empty())
  {
    // Remove previous contacts
    for(const auto contactT : contactTasks)
    {
      LOG_INFO(name() + ": Removing contact " << contactT->surface());
      MetaTask::removeFromLogger(*contactT, *solver.logger());
      MetaTask::removeFromSolver(*contactT, solver);
    }
    contactTasks.clear();

    // Add new contacts
    for(const auto contactState : addContacts_)
    {
      auto footTask = footTasks[contactState];
      LOG_INFO(name() + ": Adding contact " << footTask->surface());
      MetaTask::addToSolver(*footTask, solver);
      MetaTask::addToLogger(*footTask, *solver.logger());
      contactTasks.push_back(footTask);
    }
    addContacts_.clear();
  }
}

void StabilizerTask::update(mc_solver::QPSolver & solver)
{
  // Update contacts if they have changed
  updateContacts(solver);

  updateState(realRobots_.robot().com(), realRobots_.robot().comVelocity());

  // Run stabilizer
  run();

  MetaTask::update(*comTask, solver);
  MetaTask::update(*pelvisTask, solver);
  MetaTask::update(*torsoTask, solver);
  for(const auto footTask : contactTasks)
  {
    MetaTask::update(*footTask, solver);
  }

  t_ += dt_;
}

void StabilizerTask::addToLogger(mc_rtc::Logger & logger)
{
  // Globbal log entries added to other categories
  logger.addLogEntry("perf_" + name_, [this]() { return runTime_; });

  logger.addLogEntry(name_ + "_error_dcm_average", [this]() { return dcmAverageError_; });
  logger.addLogEntry(name_ + "_error_dcm_pos", [this]() { return dcmError_; });
  logger.addLogEntry(name_ + "_error_dcm_vel", [this]() { return dcmVelError_; });
  logger.addLogEntry(name_ + "_error_dfz_force", [this]() { return dfzForceError_; });
  logger.addLogEntry(name_ + "_error_dfz_height", [this]() { return dfzHeightError_; });
  logger.addLogEntry(name_ + "_error_vdc", [this]() { return vdcHeightError_; });
  logger.addLogEntry(name_ + "_error_zmp", [this]() { return zmpError_; });
  logger.addLogEntry(name_ + "_admittance_cop", [this]() { return c_.copAdmittance; });
  logger.addLogEntry(name_ + "_admittance_dfz", [this]() { return c_.dfzAdmittance; });
  logger.addLogEntry(name_ + "_dcmDerivator_filtered", [this]() { return dcmDerivator_.eval(); });
  logger.addLogEntry(name_ + "_dcmDerivator_timeConstant", [this]() { return dcmDerivator_.timeConstant(); });
  logger.addLogEntry(name_ + "_dcmIntegrator_timeConstant", [this]() { return dcmIntegrator_.timeConstant(); });
  logger.addLogEntry(name_ + "_dcmTracking_derivGain", [this]() { return c_.dcmDerivGain; });
  logger.addLogEntry(name_ + "_dcmTracking_integralGain", [this]() { return c_.dcmIntegralGain; });
  logger.addLogEntry(name_ + "_dcmTracking_propGain", [this]() { return c_.dcmPropGain; });
  logger.addLogEntry(name_ + "_dfz_damping", [this]() { return c_.dfzDamping; });
  logger.addLogEntry(name_ + "_fdqp_weights_ankleTorque",
                     [this]() { return std::pow(c_.fdqpWeights.ankleTorqueSqrt, 2); });
  logger.addLogEntry(name_ + "_fdqp_weights_netWrench", [this]() { return std::pow(c_.fdqpWeights.netWrenchSqrt, 2); });
  logger.addLogEntry(name_ + "_fdqp_weights_pressure", [this]() { return std::pow(c_.fdqpWeights.pressureSqrt, 2); });
  logger.addLogEntry(name_ + "_vdc_frequency", [this]() { return c_.vdcFrequency; });
  logger.addLogEntry(name_ + "_vdc_stiffness", [this]() { return c_.vdcStiffness; });
  logger.addLogEntry(name_ + "_wrench", [this]() { return distribWrench_; });
  logger.addLogEntry(name_ + "_support_min", [this]() { return supportMin_; });
  logger.addLogEntry(name_ + "_support_max", [this]() { return supportMax_; });
  logger.addLogEntry(name_ + "_left_foot_ratio", [this]() { return leftFootRatio_; });

  // Stabilizer targets
  logger.addLogEntry(name_ + "_target_pendulum_com", [this]() { return comTarget_; });
  logger.addLogEntry(name_ + "_target_pendulum_comd", [this]() { return comdTarget_; });
  logger.addLogEntry(name_ + "_target_pendulum_comdd", [this]() { return comddTarget_; });
  logger.addLogEntry(name_ + "_target_pendulum_dcm", [this]() { return dcmTarget_; });
  logger.addLogEntry(name_ + "_target_pendulum_omega", [this]() { return omega_; });
  logger.addLogEntry(name_ + "_target_pendulum_zmp", [this]() { return zmpTarget_; });

  logger.addLogEntry(name_ + "_contactState", [this]() -> double {
    if(inDoubleSupport())
      return 0;
    else if(inContact(ContactState::Left))
      return 1;
    else if(inContact(ContactState::Right))
      return -1;
    else
      return -3;
  });

  // Log computed robot properties
  logger.addLogEntry(name_ + "_controlRobot_LeftFoot", [this]() { return robot().surfacePose("LeftFoot"); });
  logger.addLogEntry(name_ + "_controlRobot_LeftFootCenter",
                     [this]() { return robot().surfacePose("LeftFootCenter"); });
  logger.addLogEntry(name_ + "_controlRobot_RightFoot", [this]() { return robot().surfacePose("RightFoot"); });
  logger.addLogEntry(name_ + "_controlRobot_RightFootCenter",
                     [this]() { return robot().surfacePose("RightFootCenter"); });
  logger.addLogEntry(name_ + "_controlRobot_com", [this]() { return robot().com(); });
  logger.addLogEntry(name_ + "_controlRobot_comd", [this]() { return robot().comVelocity(); });
  logger.addLogEntry(name_ + "_controlRobot_posW", [this]() { return robot().posW(); });

  logger.addLogEntry(name_ + "_realRobot_LeftFoot", [this]() { return realRobot().surfacePose("LeftFoot"); });
  logger.addLogEntry(name_ + "_realRobot_LeftFootCenter",
                     [this]() { return realRobot().surfacePose("LeftFootCenter"); });
  logger.addLogEntry(name_ + "_realRobot_RightFoot", [this]() { return realRobot().surfacePose("RightFoot"); });
  logger.addLogEntry(name_ + "_realRobot_RightFootCenter",
                     [this]() { return realRobot().surfacePose("RightFootCenter"); });
  logger.addLogEntry(name_ + "_realRobot_com", [this]() { return measuredCoM_; });
  logger.addLogEntry(name_ + "_realRobot_comd", [this]() { return measuredCoMd_; });
  logger.addLogEntry(name_ + "_realRobot_dcm", [this]() -> Eigen::Vector3d { return measuredDCM_; });
  logger.addLogEntry(name_ + "_realRobot_posW", [this]() { return realRobot().posW(); });
  logger.addLogEntry(name_ + "_realRobot_wrench", [this]() { return measuredNetWrench_; });
  logger.addLogEntry(name_ + "_realRobot_zmp", [this]() { return measuredZMP_; });

  MetaTask::addToLogger(*comTask, logger);
  MetaTask::addToLogger(*pelvisTask, logger);
  MetaTask::addToLogger(*torsoTask, logger);
}

void StabilizerTask::removeFromLogger(mc_rtc::Logger & logger)
{
  logger.removeLogEntry("perf_" + name_);
  logger.removeLogEntry(name_ + "_contactState");
  logger.removeLogEntry(name_ + "_error_dcm_average");
  logger.removeLogEntry(name_ + "_error_dcm_pos");
  logger.removeLogEntry(name_ + "_error_dcm_vel");
  logger.removeLogEntry(name_ + "_error_dfz_force");
  logger.removeLogEntry(name_ + "_error_dfz_height");
  logger.removeLogEntry(name_ + "_error_vdc");
  logger.removeLogEntry(name_ + "_error_zmp");
  logger.removeLogEntry(name_ + "_admittance_com");
  logger.removeLogEntry(name_ + "_admittance_cop");
  logger.removeLogEntry(name_ + "_admittance_dfz");
  logger.removeLogEntry(name_ + "_dcmDerivator_filtered");
  logger.removeLogEntry(name_ + "_dcmDerivator_timeConstant");
  logger.removeLogEntry(name_ + "_dcmIntegrator_timeConstant");
  logger.removeLogEntry(name_ + "_dcmTracking_derivGain");
  logger.removeLogEntry(name_ + "_dcmTracking_integralGain");
  logger.removeLogEntry(name_ + "_dcmTracking_propGain");
  logger.removeLogEntry(name_ + "_dfz_damping");
  logger.removeLogEntry(name_ + "_fdqp_weights_ankleTorque");
  logger.removeLogEntry(name_ + "_fdqp_weights_netWrench");
  logger.removeLogEntry(name_ + "_fdqp_weights_pressure");
  logger.removeLogEntry(name_ + "_vdc_frequency");
  logger.removeLogEntry(name_ + "_vdc_stiffness");
  logger.removeLogEntry(name_ + "_wrench");
  logger.removeLogEntry(name_ + "_zmp");
  logger.removeLogEntry(name_ + "_support_min");
  logger.removeLogEntry(name_ + "_support_max");
  logger.removeLogEntry(name_ + "_left_foot_ratio");
  logger.removeLogEntry(name_ + "_target_pendulum_com");
  logger.removeLogEntry(name_ + "_target_pendulum_comd");
  logger.removeLogEntry(name_ + "_target_pendulum_comdd");
  logger.removeLogEntry(name_ + "_target_pendulum_dcm");
  logger.removeLogEntry(name_ + "_target_pendulum_omega");
  logger.removeLogEntry(name_ + "_target_pendulum_zmp");
  logger.removeLogEntry(name_ + "_controlRobot_LeftFoot");
  logger.removeLogEntry(name_ + "_controlRobot_LeftFootCenter");
  logger.removeLogEntry(name_ + "_controlRobot_RightFoot");
  logger.removeLogEntry(name_ + "_controlRobot_RightFootCenter");
  logger.removeLogEntry(name_ + "_controlRobot_com");
  logger.removeLogEntry(name_ + "_controlRobot_comd");
  logger.removeLogEntry(name_ + "_controlRobot_posW");
  logger.removeLogEntry(name_ + "_realRobot_LeftFoot");
  logger.removeLogEntry(name_ + "_realRobot_LeftFootCenter");
  logger.removeLogEntry(name_ + "_realRobot_RightFoot");
  logger.removeLogEntry(name_ + "_realRobot_RightFootCenter");
  logger.removeLogEntry(name_ + "_realRobot_com");
  logger.removeLogEntry(name_ + "_realRobot_comd");
  logger.removeLogEntry(name_ + "_realRobot_dcm");
  logger.removeLogEntry(name_ + "_realRobot_posW");
  logger.removeLogEntry(name_ + "_realRobot_wrench");
  logger.removeLogEntry(name_ + "_realRobot_zmp");

  MetaTask::removeFromLogger(*comTask, logger);
  MetaTask::removeFromLogger(*pelvisTask, logger);
  MetaTask::removeFromLogger(*torsoTask, logger);
  for(const auto & footT : contactTasks)
  {
    MetaTask::removeFromLogger(*footT, logger);
  }
}

void StabilizerTask::addToGUI(mc_rtc::gui::StateBuilder & gui)
{
  using namespace mc_rtc::gui;

  gui.addElement({"Tasks", name_, "Main"}, Button("Disable stabilizer", [this]() { disable(); }),
                 Button("Reconfigure / Enable Stabilizer", [this]() { reconfigure(); }),
                 Button("Reset DCM integrator", [this]() { dcmIntegrator_.reset(Eigen::Vector3d::Zero()); }),
                 ArrayInput("Foot admittance", {"CoPx", "CoPy"},
                            [this]() -> Eigen::Vector2d {
                              return {c_.copAdmittance.x(), c_.copAdmittance.y()};
                            },
                            [this](const Eigen::Vector2d & a) { c_.copAdmittance = clamp(a, 0., MAX_COP_ADMITTANCE); }),
                 ArrayInput("Foot force difference", {"Admittance", "Damping"},
                            [this]() -> Eigen::Vector2d {
                              return {c_.dfzAdmittance, c_.dfzDamping};
                            },
                            [this](const Eigen::Vector2d & a) {
                              c_.dfzAdmittance = clamp(a(0), 0., MAX_DFZ_ADMITTANCE);
                              c_.dfzDamping = clamp(a(1), 0., MAX_DFZ_DAMPING);
                            }),
                 ArrayInput("DCM gains", {"Prop.", "Integral", "Deriv."},
                            [this]() -> Eigen::Vector3d {
                              return {c_.dcmPropGain, c_.dcmIntegralGain, c_.dcmDerivGain};
                            },
                            [this](const Eigen::Vector3d & gains) {
                              c_.dcmPropGain = clamp(gains(0), 0., MAX_DCM_P_GAIN);
                              c_.dcmIntegralGain = clamp(gains(1), 0., MAX_DCM_I_GAIN);
                              c_.dcmDerivGain = clamp(gains(2), 0., MAX_DCM_D_GAIN);
                            }),
                 ArrayInput("DCM filters", {"Integrator T [s]", "Derivator T [s]"},
                            [this]() -> Eigen::Vector2d {
                              return {dcmIntegrator_.timeConstant(), dcmDerivator_.timeConstant()};
                            },
                            [this](const Eigen::Vector2d & T) {
                              dcmIntegrator_.timeConstant(T(0));
                              dcmDerivator_.timeConstant(T(1));
                            }));
  gui.addElement({"Tasks", name_, "Advanced"}, Button("Disable stabilizer", [this]() { disable(); }),
                 Button("Reconfigure", [this]() { reconfigure(); }),
                 ArrayInput("Vertical drift compensation", {"frequency", "stiffness"},
                            [this]() -> Eigen::Vector2d {
                              return {c_.vdcFrequency, c_.vdcStiffness};
                            },
                            [this](const Eigen::Vector2d & v) {
                              c_.vdcFrequency = clamp(v(0), 0., 10.);
                              c_.vdcStiffness = clamp(v(1), 0., 1e4);
                            }),
                 NumberInput("Torso pitch [rad]", [this]() { return c_.torsoPitch; },
                             [this](double pitch) { c_.torsoPitch = pitch; }));

  gui.addElement({"Tasks", name_, "Debug"}, Button("Disable stabilizer", [this]() { disable(); }),
                 Button("Reconfigure", [this]() { reconfigure(); }), Button("Dump configuration", [this]() {
                   LOG_INFO("[LIPMStabilizerTask] configuration (YAML)");
                   LOG_INFO(c_.save().dump(true, true));
                 }));

  gui.addElement({"Tasks", name_, "Debug"}, ElementsStacking::Horizontal,
                 Button("Plot DCM-ZMP Tracking (x)",
                        [this, &gui]() {
                          gui.addPlot("DCM-ZMP Tracking (x)", plot::X("t", [this]() { return t_; }),
                                      plot::Y("support_min", [this]() { return supportMin_.x(); }, Color::Red),
                                      plot::Y("support_max", [this]() { return supportMax_.x(); }, Color::Red),
                                      plot::Y("dcm_ref", [this]() { return dcmTarget_.x(); }, Color::Red),
                                      plot::Y("dcm_mes", [this]() { return measuredDCM_.x(); }, Color::Magenta),
                                      plot::Y("zmp_ref", [this]() { return zmpTarget_.x(); }, Color::Blue),
                                      plot::Y("zmp_mes", [this]() { return measuredZMP_.x(); }, Color::Cyan));
                        }),
                 Button("Stop DCM-ZMP (x)", [&gui]() { gui.removePlot("DCM-ZMP Tracking (x)"); }));

  gui.addElement({"Tasks", name_, "Debug"}, ElementsStacking::Horizontal,
                 Button("Plot DCM-ZMP Tracking (y)",
                        [this, &gui]() {
                          gui.addPlot("DCM-ZMP Tracking (y)", plot::X("t", [this]() { return t_; }),
                                      plot::Y("support_min", [this]() { return supportMin_.y(); }, Color::Red),
                                      plot::Y("support_max", [this]() { return supportMax_.y(); }, Color::Red),
                                      plot::Y("dcm_ref", [this]() { return dcmTarget_.y(); }, Color::Red),
                                      plot::Y("dcm_mes", [this]() { return measuredDCM_.y(); }, Color::Magenta),
                                      plot::Y("zmp_ref", [this]() { return zmpTarget_.y(); }, Color::Blue),
                                      plot::Y("zmp_mes", [this]() { return measuredZMP_.y(); }, Color::Cyan));
                        }),
                 Button("Stop DCM-ZMP (y)", [&gui]() { gui.removePlot("DCM-ZMP Tracking (y)"); }));

  gui.addElement({"Tasks", name_, "Debug"}, ElementsStacking::Horizontal,
                 Button("Plot CoM Tracking (x)",
                        [this, &gui]() {
                          gui.addPlot("CoM Tracking (x)", plot::X("t", [this]() { return t_; }),
                                      plot::Y("com_ref", [this]() { return comTarget_.x(); }, Color::Red),
                                      plot::Y("com_mes", [this]() { return measuredCoM_.y(); }, Color::Magenta));
                        }),
                 Button("Stop CoM (x)", [&gui]() { gui.removePlot("CoM Tracking (x)"); }));

  gui.addElement({"Tasks", name_, "Debug"}, ElementsStacking::Horizontal,
                 Button("Plot DCM Integrator",
                        [this, &gui]() {
                          gui.addPlot("DCM Integrator", plot::X("t", [this]() { return t_; }),
                                      plot::Y("x", [this]() { return dcmIntegrator_.eval().x(); }, Color::Red),
                                      plot::Y("y", [this]() { return dcmIntegrator_.eval().y(); }, Color::Green),
                                      plot::Y("z", [this]() { return dcmIntegrator_.eval().z(); }, Color::Blue));
                        }),
                 Button("Stop DCM Integrator", [&gui]() { gui.removePlot("DCM Integrator"); }));
  gui.addElement({"Tasks", name_, "Debug"}, ElementsStacking::Horizontal,
                 Button("Plot DCM Derivator",
                        [this, &gui]() {
                          gui.addPlot("DCM Derivator", plot::X("t", [this]() { return t_; }),
                                      plot::Y("x", [this]() { return dcmDerivator_.eval().x(); }, Color::Red),
                                      plot::Y("y", [this]() { return dcmDerivator_.eval().y(); }, Color::Green),
                                      plot::Y("z", [this]() { return dcmDerivator_.eval().z(); }, Color::Blue));
                        }),
                 Button("Stop DCM Derivator", [&gui]() { gui.removePlot("DCM Derivator"); }));

  gui.addElement({"Tasks", name_, "Debug"},
                 ArrayLabel("DCM average error [mm]", {"x", "y"}, [this]() { return vecFromError(dcmAverageError_); }),
                 ArrayLabel("DCM error [mm]", {"x", "y"}, [this]() { return vecFromError(dcmError_); }),
                 ArrayLabel("Foot force difference error [mm]", {"force", "height"}, [this]() {
                   Eigen::Vector3d dfzError = {dfzForceError_, dfzHeightError_, 0.};
                   return vecFromError(dfzError);
                 }));

  ///// GUI MARKERS
  constexpr double ARROW_HEAD_DIAM = 0.015;
  constexpr double ARROW_HEAD_LEN = 0.05;
  constexpr double ARROW_SHAFT_DIAM = 0.015;
  constexpr double FORCE_SCALE = 0.0015;

  ArrowConfig pendulumArrowConfig;
  pendulumArrowConfig.color = Color::Yellow;
  pendulumArrowConfig.end_point_scale = 0.02;
  pendulumArrowConfig.head_diam = .1 * ARROW_HEAD_DIAM;
  pendulumArrowConfig.head_len = .1 * ARROW_HEAD_LEN;
  pendulumArrowConfig.scale = 1.;
  pendulumArrowConfig.shaft_diam = .1 * ARROW_SHAFT_DIAM;
  pendulumArrowConfig.start_point_scale = 0.02;

  ArrowConfig pendulumForceArrowConfig;
  pendulumForceArrowConfig.shaft_diam = 1 * ARROW_SHAFT_DIAM;
  pendulumForceArrowConfig.head_diam = 1 * ARROW_HEAD_DIAM;
  pendulumForceArrowConfig.head_len = 1 * ARROW_HEAD_LEN;
  pendulumForceArrowConfig.scale = 1.;
  pendulumForceArrowConfig.start_point_scale = 0.02;
  pendulumForceArrowConfig.end_point_scale = 0.;

  ArrowConfig netWrenchForceArrowConfig = pendulumForceArrowConfig;
  netWrenchForceArrowConfig.color = Color::Red;

  ArrowConfig refPendulumForceArrowConfig = pendulumForceArrowConfig;
  refPendulumForceArrowConfig = Color::Yellow;

  ForceConfig copForceConfig(Color::Green);
  copForceConfig.start_point_scale = 0.02;
  copForceConfig.end_point_scale = 0.;

  constexpr double COM_POINT_SIZE = 0.02;
  constexpr double DCM_POINT_SIZE = 0.015;

  gui.addElement({"Tasks", name_, "Markers", "CoM-DCM"},
                 Arrow("Pendulum_CoM", pendulumArrowConfig, [this]() -> Eigen::Vector3d { return zmpTarget_; },
                       [this]() -> Eigen::Vector3d { return comTarget_; }),
                 Point3D("Measured_CoM", PointConfig(Color::Green, COM_POINT_SIZE), [this]() { return measuredCoM_; }),
                 Point3D("Pendulum_DCM", PointConfig(Color::Yellow, DCM_POINT_SIZE), [this]() { return dcmTarget_; }),
                 Point3D("Measured_DCM", PointConfig(Color::Green, DCM_POINT_SIZE),
                         [this]() -> Eigen::Vector3d { return measuredCoM_ + measuredCoMd_ / omega_; }));

  gui.addElement(
      {"Tasks", name_, "Markers", "Net wrench"},
      Point3D("Measured_ZMP", PointConfig(Color::Red, 0.02), [this]() -> Eigen::Vector3d { return measuredZMP_; }),
      Arrow("Measured_ZMPForce", netWrenchForceArrowConfig, [this]() -> Eigen::Vector3d { return measuredZMP_; },
            [this, FORCE_SCALE]() -> Eigen::Vector3d {
              return measuredZMP_ + FORCE_SCALE * measuredNetWrench_.force();
            }));

  for(const auto footTask : footTasks)
  {
    auto footT = footTask.second;
    gui.addElement({"Tasks", name_, "Markers", "Foot wrenches"},
                   Point3D("Stabilizer_" + footT->surface() + "CoP", PointConfig(Color::Magenta, 0.01),
                           [footT]() { return footT->targetCoPW(); }),
                   Force("Measured_" + footT->surface() + "CoPForce", copForceConfig,
                         [footT]() { return footT->measuredWrench(); },
                         [footT]() { return sva::PTransformd(footT->measuredCoPW()); }));
  }

  gui.addElement({"Tasks", name_, "Markers", "Contacts"},
                 Polygon("SupportContacts", Color::Green, [this]() { return supportPolygons_; }));
}

void StabilizerTask::removeFromGUI(mc_rtc::gui::StateBuilder & gui)
{
  MetaTask::removeFromGUI(gui);
  gui.removePlot("DCM-ZMP Tracking (x)");
  gui.removePlot("DCM-ZMP Tracking (y)");
  gui.removePlot("CoM Tracking (x)");
  gui.removePlot("DCM Integrator");
}

void StabilizerTask::enable()
{
  // Reset DCM integrator when enabling the stabilizer.
  // While idle, it will accumulate a lot of error, and would case the robot to
  // move suddently to compensate it otherwise
  dcmIntegrator_.reset(Eigen::Vector3d::Zero());
  dcmDerivator_.reset(Eigen::Vector3d::Zero());

  reconfigure();
}

void StabilizerTask::disable()
{
  c_.copAdmittance.setZero();
  c_.dcmDerivGain = 0.;
  c_.dcmIntegralGain = 0.;
  c_.dcmPropGain = 0.;
  c_.dfzAdmittance = 0.;
  c_.vdcFrequency = 0.;
  c_.vdcStiffness = 0.;
}

void StabilizerTask::reconfigure()
{
  configure(defaultConfig_);
}

void StabilizerTask::configure(const mc_rbdyn::lipm_stabilizer::StabilizerConfiguration & config)
{
  defaultConfig_ = config;
  c_ = defaultConfig_;

  dcmDerivator_.timeConstant(c_.dcmDerivatorTimeConstant);
  dcmIntegrator_.timeConstant(c_.dcmIntegratorTimeConstant);

  // Configure upper-body tasks
  pelvisTask->stiffness(c_.pelvisStiffness);
  pelvisTask->weight(c_.pelvisWeight);

  torsoTask->stiffness(c_.torsoStiffness);
  torsoTask->weight(c_.torsoWeight);
  torsoTask->orientation(mc_rbdyn::rpyToMat({0, c_.torsoPitch, 0}));

  if(!c_.comActiveJoints.empty())
  {
    comTask->selectActiveJoints(c_.comActiveJoints);
  }
  comTask->setGains(c_.comStiffness, 2 * c_.comStiffness.cwiseSqrt());
  comTask->weight(c_.comWeight);

  for(const auto & footTask : footTasks)
  {
    footTask.second->maxLinearVel(c_.copMaxVel.linear());
    footTask.second->maxAngularVel(c_.copMaxVel.angular());
  }
}

void StabilizerTask::load(mc_solver::QPSolver &, const mc_rtc::Configuration & config)
{
  double height = 0;
  // Load contacts
  std::vector<std::pair<ContactState, Contact>> contactsToAdd;
  if(config.has("contacts"))
  {
    const auto & contacts = config("contacts");
    for(const auto & contactName : contacts)
    {
      ContactState s = contactName;
      sva::PTransformd contactPose = footTasks[s]->surfacePose();
      if(config.has(contactName))
      {
        const auto & c = config(contactName);
        if(c.has("rotation"))
        {
          contactPose.rotation() = c("rotation");
        }
        if(c.has("translation"))
        {
          contactPose.translation() = c("translation");
        }
        if(c.has("height"))
        {
          double h = c("height");
          contactPose.translation().z() = c("height");
          height = (height + h) / 2;
        }
      }
      contactsToAdd.push_back({s, {robot(), footTasks[s]->surface(), contactPose, c_.friction}});
    }
  }
  this->setContacts(contactsToAdd);

  // Target robot com by default
  Eigen::Vector3d comTarget = robot().com();
  if(config.has("staticTarget"))
  {
    if(config.has("com"))
    {
      comTarget = config("staticTarget")("com");
    }
  }
  this->staticTarget(comTarget, height);

  // Allow to start in disabled state
  if(!config("enabled", true))
  {
    this->disable();
  }
}

const mc_rbdyn::lipm_stabilizer::StabilizerConfiguration & StabilizerTask::config() const
{
  return c_;
}

const mc_rbdyn::lipm_stabilizer::StabilizerConfiguration & StabilizerTask::commitedConfig() const
{
  return defaultConfig_;
}

void StabilizerTask::checkGains()
{
  clampInPlaceAndWarn(c_.copAdmittance.x(), 0., MAX_COP_ADMITTANCE, "CoP x-admittance");
  clampInPlaceAndWarn(c_.copAdmittance.y(), 0., MAX_COP_ADMITTANCE, "CoP y-admittance");
  clampInPlaceAndWarn(c_.dcmDerivGain, 0., MAX_DCM_D_GAIN, "DCM deriv x-gain");
  clampInPlaceAndWarn(c_.dcmIntegralGain, 0., MAX_DCM_I_GAIN, "DCM integral x-gain");
  clampInPlaceAndWarn(c_.dcmPropGain, 0., MAX_DCM_P_GAIN, "DCM prop x-gain");
  clampInPlaceAndWarn(c_.dfzAdmittance, 0., MAX_DFZ_ADMITTANCE, "DFz admittance");
}

void StabilizerTask::setContacts(const std::vector<ContactState> & contacts)
{
  std::vector<std::pair<ContactState, Contact>> addContacts;
  for(const auto contact : contacts)
  {
    addContacts.push_back({contact,
                           {robot(), footTasks[contact]->surface(),
                            realRobot().surfacePose(footTasks[contact]->surface()), c_.friction}});
  }
  setContacts(addContacts);
}

void StabilizerTask::setContacts(const std::vector<std::pair<ContactState, sva::PTransformd>> & contacts)
{
  std::vector<std::pair<ContactState, Contact>> addContacts;
  for(const auto contact : contacts)
  {
    addContacts.push_back({contact.first, {robot(), footTasks[contact.first]->surface(), contact.second, c_.friction}});
  }
  setContacts(addContacts);
}

void StabilizerTask::setContacts(const std::vector<std::pair<ContactState, Contact>> & contacts)
{
  if(contacts.empty())
  {
    LOG_ERROR_AND_THROW(std::runtime_error, "[StabilizerTask] Cannot set contacts from an empty list, the stabilizer "
                                            "requires at least one contact to be set.");
  }
  contacts_.clear();

  // Reset support area boundaries
  supportMin_ = std::numeric_limits<double>::max() * Eigen::Vector2d::Ones();
  supportMax_ = -supportMin_;
  supportPolygons_.clear();

  for(const auto & contact : contacts)
  {
    addContact(contact.first, contact.second);
  }
}

void StabilizerTask::addContact(ContactState contactState, const Contact & contact)
{
  auto footTask = footTasks[contactState];
  // Use real robot's surface pose as contact
  contacts_.emplace(std::make_pair(contactState, contact));

  supportMin_.x() = std::min(contact.xmin(), supportMin_.x());
  supportMin_.y() = std::min(contact.ymin(), supportMin_.y());
  supportMax_.x() = std::max(contact.xmax(), supportMax_.x());
  supportMax_.y() = std::max(contact.ymax(), supportMax_.y());

  supportPolygons_.push_back(contact.polygon());

  // Configure support foot task
  footTask->reset();
  footTask->weight(c_.contactWeight);
  footTask->targetPose(contact.surfacePose());

  addContacts_.push_back(contactState);
}

void StabilizerTask::setSupportFootGains()
{
  if(inDoubleSupport())
  {
    for(auto contactT : contactTasks)
    {
      contactT->admittance(contactAdmittance());
      contactT->setGains(c_.contactStiffness, c_.contactDamping);
    }
  }
  else
  {
    sva::MotionVecd vdcContactStiffness = {c_.contactStiffness.angular(),
                                           {c_.vdcStiffness, c_.vdcStiffness, c_.vdcStiffness}};
    for(auto contactT : contactTasks)
    {
      contactT->admittance(contactAdmittance());
      contactT->setGains(vdcContactStiffness, c_.contactDamping);
    }
  }
}

void StabilizerTask::checkInTheAir()
{
  inTheAir_ = true;
  for(const auto footT : footTasks)
  {
    inTheAir_ = inTheAir_ && footT.second->measuredWrench().force().z() < MIN_DS_PRESSURE;
  }
}

void StabilizerTask::computeLeftFootRatio()
{
  if(inDoubleSupport())
  {
    // Project desired CoM in-between foot-sole ankle frames and compute ratio along the line in-beween the two surfaces
    const Eigen::Vector3d & lankle = contacts_.at(ContactState::Left).anklePose().translation();
    const Eigen::Vector3d & rankle = contacts_.at(ContactState::Right).anklePose().translation();
    Eigen::Vector3d t_lankle_com = comTarget_ - lankle;
    Eigen::Vector3d t_lankle_rankle = rankle - lankle;
    double d_proj = t_lankle_com.dot(t_lankle_rankle.normalized());
    leftFootRatio_ = clamp(d_proj / t_lankle_rankle.norm(), 0., 1.);
  }
  else if(inContact(ContactState::Left))
  {
    leftFootRatio_ = 0;
  }
  else
  {
    leftFootRatio_ = 1;
  }
}

sva::PTransformd StabilizerTask::anchorFrame() const
{
  return sva::interpolate(robot().surfacePose(footTasks.at(ContactState::Left)->surface()),
                          robot().surfacePose(footTasks.at(ContactState::Right)->surface()), leftFootRatio_);
}

sva::PTransformd StabilizerTask::anchorFrameReal() const
{
  return sva::interpolate(realRobot().surfacePose(footTasks.at(ContactState::Left)->surface()),
                          realRobot().surfacePose(footTasks.at(ContactState::Right)->surface()), leftFootRatio_);
}

void StabilizerTask::updateZMPFrame()
{
  if(inDoubleSupport())
  {
    zmpFrame_ = sva::interpolate(contacts_.at(ContactState::Left).surfacePose(),
                                 contacts_.at(ContactState::Right).surfacePose(), 0.5);
  }
  else if(inContact(ContactState::Left))
  {
    zmpFrame_ = contacts_.at(ContactState::Left).surfacePose();
  }
  else
  {
    zmpFrame_ = contacts_.at(ContactState::Right).surfacePose();
  }
  measuredNetWrench_ = robots_.robot(robotIndex_).netWrench(sensorNames_);
  measuredZMP_ = robots_.robot(robotIndex_).zmp(measuredNetWrench_, zmpFrame_, MIN_NET_TOTAL_FORCE_ZMP);
}

void StabilizerTask::staticTarget(const Eigen::Vector3d & com, double zmpHeight)
{
  Eigen::Vector3d zmp = Eigen::Vector3d{com.x(), com.y(), zmpHeight};

  target(com, Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero(), zmp);
}

void StabilizerTask::target(const Eigen::Vector3d & com,
                            const Eigen::Vector3d & comd,
                            const Eigen::Vector3d & comdd,
                            const Eigen::Vector3d & zmp)
{
  comTarget_ = com;
  comdTarget_ = comd;
  comddTarget_ = comdd;
  zmpTarget_ = zmp;
  omega_ = std::sqrt(constants::gravity.z() / comTarget_.z());
  dcmTarget_ = comTarget_ + comdTarget_ / omega_;
}

void StabilizerTask::run()
{
  using namespace std::chrono;
  using clock = typename std::conditional<std::chrono::high_resolution_clock::is_steady,
                                          std::chrono::high_resolution_clock, std::chrono::steady_clock>::type;
  auto startTime = clock::now();

  checkGains();
  checkInTheAir();
  computeLeftFootRatio();
  setSupportFootGains();
  updateZMPFrame();
  auto desiredWrench = computeDesiredWrench();

  if(inDoubleSupport())
  {
    distributeWrench(desiredWrench);
  }
  else if(inContact(ContactState::Left))
  {
    saturateWrench(desiredWrench, footTasks[ContactState::Left], contacts_.at(ContactState::Left));
    footTasks[ContactState::Right]->setZeroTargetWrench();
  }
  else
  {
    saturateWrench(desiredWrench, footTasks[ContactState::Right], contacts_.at(ContactState::Right));
    footTasks[ContactState::Left]->setZeroTargetWrench();
  }

  updateFootForceDifferenceControl();

  comTask->com(comTarget_);
  comTask->refVel(comdTarget_);
  comTask->refAccel(comddTarget_);

  // Update orientation tasks according to feet orientation
  sva::PTransformd X_0_a = anchorFrame();
  Eigen::Matrix3d pelvisOrientation = X_0_a.rotation();
  pelvisTask->orientation(pelvisOrientation);
  torsoTask->orientation(mc_rbdyn::rpyToMat({0, c_.torsoPitch, 0}) * pelvisOrientation);

  auto endTime = clock::now();
  runTime_ = 1000. * duration_cast<duration<double>>(endTime - startTime).count();
}

void StabilizerTask::updateState(const Eigen::Vector3d & com, const Eigen::Vector3d & comd)
{
  measuredCoM_ = com;
  measuredCoMd_ = comd;
  measuredDCM_ = measuredCoM_ + measuredCoMd_ / omega_;
}

sva::ForceVecd StabilizerTask::computeDesiredWrench()
{
  Eigen::Vector3d comError = comTarget_ - measuredCoM_;
  Eigen::Vector3d comdError = comdTarget_ - measuredCoMd_;
  dcmError_ = comError + comdError / omega_;
  dcmError_.z() = 0.;

  if(inTheAir_)
  {
    dcmDerivator_.reset(Eigen::Vector3d::Zero());
    dcmIntegrator_.append(Eigen::Vector3d::Zero());
  }
  else
  {
    zmpError_ = zmpTarget_ - measuredZMP_; // XXX: both in same plane?
    zmpError_.z() = 0.;
    dcmDerivator_.update(omega_ * (dcmError_ - zmpError_));
    dcmIntegrator_.append(dcmError_);
  }
  dcmAverageError_ = dcmIntegrator_.eval();
  dcmVelError_ = dcmDerivator_.eval();

  Eigen::Vector3d desiredCoMAccel = comddTarget_;
  desiredCoMAccel += omega_ * (c_.dcmPropGain * dcmError_ + comdError);
  desiredCoMAccel += omega_ * c_.dcmIntegralGain * dcmAverageError_;
  desiredCoMAccel += omega_ * c_.dcmDerivGain * dcmVelError_;
  auto desiredForce = mass_ * (desiredCoMAccel + constants::gravity);

  // Previous implementation (up to v1.3):
  // return {pendulum_.com().cross(desiredForce), desiredForce};
  // See https://github.com/stephane-caron/lipm_walking_controller/issues/28
  return {measuredCoM_.cross(desiredForce), desiredForce};
}

void StabilizerTask::distributeWrench(const sva::ForceVecd & desiredWrench)
{
  // Variables
  // ---------
  // x = [w_l_0 w_r_0] where
  // w_l_0: spatial force vector of left foot contact in inertial frame
  // w_r_0: spatial force vector of right foot contact in inertial frame
  //
  // Objective
  // ---------
  // Weighted minimization of the following tasks:
  // w_l_0 + w_r_0 == desiredWrench  -- realize desired contact wrench
  // w_l_lankle == 0 -- minimize left foot ankle torque (anisotropic weight)
  // w_r_rankle == 0 -- minimize right foot ankle torque (anisotropic weight)
  // (1 - lfr) * w_l_lc.z() == lfr * w_r_rc.z()
  //
  // Constraints
  // -----------
  // CWC X_0_lc* w_l_0 <= 0  -- left foot wrench within contact wrench cone
  // CWC X_0_rc* w_r_0 <= 0  -- right foot wrench within contact wrench cone
  // (X_0_lc* w_l_0).z() > minPressure  -- minimum left foot contact pressure
  // (X_0_rc* w_r_0).z() > minPressure  -- minimum right foot contact pressure

  const auto & leftContact = contacts_.at(ContactState::Left);
  const auto & rightContact = contacts_.at(ContactState::Right);
  const sva::PTransformd & X_0_lc = leftContact.surfacePose();
  const sva::PTransformd & X_0_rc = rightContact.surfacePose();
  const sva::PTransformd & X_0_lankle = leftContact.anklePose();
  const sva::PTransformd & X_0_rankle = rightContact.anklePose();

  constexpr unsigned NB_VAR = 6 + 6;
  constexpr unsigned COST_DIM = 6 + NB_VAR + 1;
  Eigen::MatrixXd A;
  Eigen::VectorXd b;
  A.setZero(COST_DIM, NB_VAR);
  b.setZero(COST_DIM);

  // |w_l_0 + w_r_0 - desiredWrench|^2
  auto A_net = A.block<6, 12>(0, 0);
  auto b_net = b.segment<6>(0);
  A_net.block<6, 6>(0, 0) = Eigen::Matrix6d::Identity();
  A_net.block<6, 6>(0, 6) = Eigen::Matrix6d::Identity();
  b_net = desiredWrench.vector();

  // |ankle torques|^2
  auto A_lankle = A.block<6, 6>(6, 0);
  auto A_rankle = A.block<6, 6>(12, 6);
  // anisotropic weights:  taux, tauy, tauz,   fx,   fy,   fz;
  A_lankle.diagonal() << 1., 1., 1e-4, 1e-3, 1e-3, 1e-4;
  A_rankle.diagonal() << 1., 1., 1e-4, 1e-3, 1e-3, 1e-4;
  A_lankle *= X_0_lankle.dualMatrix();
  A_rankle *= X_0_rankle.dualMatrix();

  // |(1 - lfr) * w_l_lc.force().z() - lfr * w_r_rc.force().z()|^2
  double lfr = leftFootRatio_;
  auto A_pressure = A.block<1, 12>(18, 0);
  A_pressure.block<1, 6>(0, 0) = (1 - lfr) * X_0_lc.dualMatrix().bottomRows<1>();
  A_pressure.block<1, 6>(0, 6) = -lfr * X_0_rc.dualMatrix().bottomRows<1>();

  // Apply weights
  A_net *= c_.fdqpWeights.netWrenchSqrt;
  b_net *= c_.fdqpWeights.netWrenchSqrt;
  A_lankle *= c_.fdqpWeights.ankleTorqueSqrt;
  A_rankle *= c_.fdqpWeights.ankleTorqueSqrt;
  // b_lankle = 0
  // b_rankle = 0
  A_pressure *= c_.fdqpWeights.pressureSqrt;
  // b_pressure = 0

  Eigen::MatrixXd Q = A.transpose() * A;
  Eigen::VectorXd c = -A.transpose() * b;

  constexpr unsigned NB_CONS = 16 + 16 + 2;
  Eigen::Matrix<double, NB_CONS, NB_VAR> A_ineq;
  Eigen::VectorXd b_ineq;
  A_ineq.setZero(NB_CONS, NB_VAR);
  b_ineq.setZero(NB_CONS);
  // CWC * w_l_lc <= 0
  A_ineq.block<16, 6>(0, 0) = leftContact.wrenchFaceMatrix() * X_0_lc.dualMatrix();
  // b_ineq.segment<16>(0) is already zero
  // CWC * w_r_rc <= 0
  A_ineq.block<16, 6>(16, 6) = rightContact.wrenchFaceMatrix() * X_0_rc.dualMatrix();
  // b_ineq.segment<16>(16) is already zero
  // w_l_lc.force().z() >= MIN_DS_PRESSURE
  A_ineq.block<1, 6>(32, 0) = -X_0_lc.dualMatrix().bottomRows<1>();
  b_ineq(32) = -MIN_DS_PRESSURE;
  // w_r_rc.force().z() >= MIN_DS_PRESSURE
  A_ineq.block<1, 6>(33, 6) = -X_0_rc.dualMatrix().bottomRows<1>();
  b_ineq(33) = -MIN_DS_PRESSURE;

  qpSolver_.problem(NB_VAR, 0, NB_CONS);
  Eigen::MatrixXd A_eq(0, 0);
  Eigen::VectorXd b_eq;
  b_eq.resize(0);

  bool solutionFound = qpSolver_.solve(Q, c, A_eq, b_eq, A_ineq, b_ineq, /* isDecomp = */ false);
  if(!solutionFound)
  {
    LOG_ERROR("[StabilizerTask] DS force distribution QP: solver found no solution");
    return;
  }

  Eigen::VectorXd x = qpSolver_.result();
  sva::ForceVecd w_l_0(x.segment<3>(0), x.segment<3>(3));
  sva::ForceVecd w_r_0(x.segment<3>(6), x.segment<3>(9));
  distribWrench_ = w_l_0 + w_r_0;

  sva::ForceVecd w_l_lc = X_0_lc.dualMul(w_l_0);
  sva::ForceVecd w_r_rc = X_0_rc.dualMul(w_r_0);
  Eigen::Vector2d leftCoP = (constants::vertical.cross(w_l_lc.couple()) / w_l_lc.force()(2)).head<2>();
  Eigen::Vector2d rightCoP = (constants::vertical.cross(w_r_rc.couple()) / w_r_rc.force()(2)).head<2>();
  footTasks[ContactState::Left]->targetCoP(leftCoP);
  footTasks[ContactState::Left]->targetForce(w_l_lc.force());
  footTasks[ContactState::Right]->targetCoP(rightCoP);
  footTasks[ContactState::Right]->targetForce(w_r_rc.force());
}

void StabilizerTask::saturateWrench(const sva::ForceVecd & desiredWrench,
                                    std::shared_ptr<mc_tasks::force::CoPTask> & footTask,
                                    const Contact & contact)
{
  constexpr unsigned NB_CONS = 16;
  constexpr unsigned NB_VAR = 6;

  // Variables
  // ---------
  // x = [w_0] where
  // w_0: spatial force vector of foot contact in inertial frame
  //
  // Objective
  // ---------
  // weighted minimization of |w_c - X_0_c* desiredWrench|^2
  //
  // Constraints
  // -----------
  // F X_0_c* w_0 <= 0    -- contact stability

  const sva::PTransformd & X_0_c = contact.surfacePose();

  Eigen::Matrix6d Q = Eigen::Matrix6d::Identity();
  Eigen::Vector6d c = -desiredWrench.vector();

  Eigen::MatrixXd A_ineq = contact.wrenchFaceMatrix() * X_0_c.dualMatrix();
  Eigen::VectorXd b_ineq;
  b_ineq.setZero(NB_CONS);

  qpSolver_.problem(NB_VAR, 0, NB_CONS);
  Eigen::MatrixXd A_eq(0, 0);
  Eigen::VectorXd b_eq;
  b_eq.resize(0);

  bool solutionFound = qpSolver_.solve(Q, c, A_eq, b_eq, A_ineq, b_ineq, /* isDecomp = */ true);
  if(!solutionFound)
  {
    LOG_ERROR("[StabilizerTask] SS force distribution QP: solver found no solution");
    return;
  }

  Eigen::VectorXd x = qpSolver_.result();
  sva::ForceVecd w_0(x.head<3>(), x.tail<3>());
  sva::ForceVecd w_c = X_0_c.dualMul(w_0);
  Eigen::Vector2d cop = (constants::vertical.cross(w_c.couple()) / w_c.force()(2)).head<2>();
  footTask->targetCoP(cop);
  footTask->targetForce(w_c.force());
  distribWrench_ = w_0;
}

void StabilizerTask::updateFootForceDifferenceControl()
{
  auto leftFootTask = footTasks[ContactState::Left];
  auto rightFootTask = footTasks[ContactState::Right];
  if(!inDoubleSupport() || inTheAir_)
  {
    dfzForceError_ = 0.;
    dfzHeightError_ = 0.;
    vdcHeightError_ = 0.;
    leftFootTask->refVelB({{0., 0., 0.}, {0., 0., 0.}});
    rightFootTask->refVelB({{0., 0., 0.}, {0., 0., 0.}});
    return;
  }

  double LFz_d = leftFootTask->targetWrench().force().z();
  double RFz_d = rightFootTask->targetWrench().force().z();
  double LFz = leftFootTask->measuredWrench().force().z();
  double RFz = rightFootTask->measuredWrench().force().z();
  dfzForceError_ = (LFz_d - RFz_d) - (LFz - RFz);

  double LTz_d = leftFootTask->targetPose().translation().z();
  double RTz_d = rightFootTask->targetPose().translation().z();
  double LTz = leftFootTask->surfacePose().translation().z();
  double RTz = rightFootTask->surfacePose().translation().z();
  dfzHeightError_ = (LTz_d - RTz_d) - (LTz - RTz);
  vdcHeightError_ = (LTz_d + RTz_d) - (LTz + RTz);

  double dz_ctrl = c_.dfzAdmittance * dfzForceError_ - c_.dfzDamping * dfzHeightError_;
  double dz_vdc = c_.vdcFrequency * vdcHeightError_;
  sva::MotionVecd velF = {{0., 0., 0.}, {0., 0., dz_ctrl}};
  sva::MotionVecd velT = {{0., 0., 0.}, {0., 0., dz_vdc}};
  leftFootTask->refVelB(0.5 * (velT - velF));
  rightFootTask->refVelB(0.5 * (velT + velF));
}

} // namespace lipm_stabilizer
} // namespace mc_tasks

namespace
{
static auto registered = mc_tasks::MetaTaskLoader::register_load_function(
    "lipm_stabilizer",
    [](mc_solver::QPSolver & solver, const mc_rtc::Configuration & config) {
      unsigned robotIndex = config("robotIndex", 0u);
      const auto & robot = solver.robots().robot(robotIndex);

      // Load default configuration from robot module
      auto stabiConf = robot.module().defaultLIPMStabilizerConfiguration();
      // Load user-specified configuration
      stabiConf.load(config);
      // Load user-specified stabilizer configuration for this robot
      if(config.has(robot.name()))
      {
        stabiConf.load(config(robot.name()));
      }

      auto t = std::make_shared<mc_tasks::lipm_stabilizer::StabilizerTask>(
          solver.robots(), solver.realRobots(), robotIndex, stabiConf.leftFootSurface, stabiConf.rightFootSurface,
          stabiConf.torsoBodyName, solver.dt());
      t->reset();
      t->configure(stabiConf);
      t->load(solver, config);
      return t;
    });
}
