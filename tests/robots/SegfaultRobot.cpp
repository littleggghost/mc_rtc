/*! This library create function throws */
#include <mc_rbdyn/RobotModule.h>
#include <mc_rtc/config.h>

struct SegfaultRobot : public mc_rbdyn::RobotModule
{
  SegfaultRobot()
  : RobotModule(std::string(mc_rtc::MC_ENV_DESCRIPTION_PATH),
                std::string("ground"))
  {
    *(int*)0 = 0;
  }
};

extern "C"
{
  ROBOT_MODULE_API const char * CLASS_NAME() { return "SegfaultRobot"; }
  ROBOT_MODULE_API void destroy(mc_rbdyn::RobotModule * ptr)
  {
    delete ptr;
  }
  ROBOT_MODULE_API mc_rbdyn::RobotModule * create()
  {
    return new SegfaultRobot();
  }
}