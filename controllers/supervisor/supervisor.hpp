#ifndef H_SUPERVISOR_HPP
#define H_SUPERVISOR_HPP
#pragma once

#include "constants.hpp"

#include <webots/Camera.hpp>
#include <webots/Supervisor.hpp>

#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <cassert>
#include <cmath>

namespace /* anonymous */ {

  std::string generate_ball_node_string(double x, double y, double z, double radius)
  {
    using namespace constants;

    // It is well known that stringstream is horribly slow.
    // Will it affect the performance? I don't think so.
    std::stringstream ss;
    ss << "DEF " << DEF_BALL << " SoccerBall {"
       << "  translation " << x << " " << y << " " << z
       << "  radius " << radius
       << "  contactMaterial \"ball\""
       << "  shapes ["
       << "    DEF " << DEF_BALLSHAPE << " SoccerBallShape {"
       << "      radius " << radius
       << "    }"
       << "    DEF " << DEF_ORANGESHAPE << " SoccerBallOrangeShape {"
       << "      radius " << radius
       << "    }"
       << "  ]"
       << "}";
    return ss.str();
  }

  std::string robot_name(bool is_red_team, std::size_t id)
  {
    return constants::DEF_ROBOT_PREFIX + (is_red_team ? "R" : "B") + std::to_string(id);
  }

  std::string generate_robot_node_string(double x, double y, double z, double th,
                                         bool is_red_team, std::size_t id)
  {
    using namespace constants;

    std::stringstream ss;
    ss << "DEF " << robot_name(is_red_team, id) << " SoccerRobot"
       << "{"
       << "  translation " << x << " " << y << " " << z
       << "  rotation 0 1 0 " << th - PI / 2 // Differential wheel faces to -z direction
       << "  name \"" << (is_red_team ? "R" : "B") << id << "\""
       << "  data \"0 0\""
       << "  controller \"soccer_robot\""
       << "  maxSpeed " << MAX_LINEAR_VELOCITY / WHEEL_RADIUS
       << "  maxForce " << MAX_FORCE
       << "  slipNoise " << SLIP_NOISE
       << "  bodyPhysics Physics {"
       << "    density -1"
       << "    mass " << BODY_MASS
       << "    centerOfMass [ 0 -0.0375 0 ]"
       << "    inertiaMatrix [ 0.01 0.000421875 0.000421875 0 0 0 ]"
       << "  }"
       << "  wheelPhysics Physics {"
       << "    density -1"
       << "    mass " << WHEEL_MASS
       << "  }"
       << "  bodyContactMaterial \"body\""
       << "  wheelContactMaterial \"wheel\""
       << "  patches ["
       << "    SoccerRobotNumberPatch {" // number patch
       << "      id " << id
       << "      isTeamTagRed " << (is_red_team ? "TRUE" : "FALSE")
       << "    }"
       << "    SoccerRobotIDPatch {" // id patch to cam_a
       << "      id " << CODEWORDS[id]
       << "      isTeamTagRed " << (is_red_team ? "TRUE" : "FALSE")
       << "    }"
       << "    SoccerRobotIDPatch {" // id patch to cam_b
       << "      id " << CODEWORDS[id]
       << "      isTeamTagRed " << (!is_red_team ? "TRUE" : "FALSE")
       << "    }"
       << "  ]"
       << "}";
    return ss.str();
  }

} // namespace /* anonymous */


class supervisor
  : public webots::Supervisor
{
  enum { T_RED, T_BLUE }; // team
  enum { N_VIEWPOINT, N_CAMA, N_CAMB }; // node
  enum { C_CAMA, C_CAMB }; // cam

public:
  supervisor()
    : pn_cams_{getFromDef(constants::DEF_AUDVIEW), getFromDef(constants::DEF_CAMA), getFromDef(constants::DEF_CAMB)}
    , pc_cams_{getCamera(constants::NAME_CAMA), getCamera(constants::NAME_CAMB)}
  {
    constexpr auto is_null = [](const auto* p) { return !p; };

    // check if cam nodes and cam devices exist
    if(std::any_of(std::cbegin(pn_cams_), std::cend(pn_cams_), is_null)
       || std::any_of(std::cbegin(pc_cams_), std::cend(pc_cams_), is_null)) {
      throw std::runtime_error("No mandatory cam nodes exists in world");
    }

    // remove existing ball and robots and place them
    remove_ball_and_robots();
    place_ball_and_robots();

    // get Node* of ball and robots and check if it's not null
    if(is_null(pn_ball_ = getFromDef(constants::DEF_BALL))) {
      throw std::runtime_error("No ball in world");
    }
    for(int t = 0; t < 2; ++t) {
      for(int id = 0; id < 5; ++id) {
        if(is_null(pn_robots_[t][id] = getFromDef(robot_name(t==T_RED, id)))) {
          throw std::runtime_error("No robot in world");
        }
      }
    }

    // control visibility to cams
    control_visibility();

    reset_position();
    enable_cameras(constants::CAM_PERIOD_MS);
  }

  // void run()
  // {
  //   while(step(1000) != -1) {
  //     const auto p = get_robot_posture(true, 0);
  //     std::cout << "x: " << std::get<0>(p) << ", y: " << std::get<1>(p) << ", th: " << std::get<2>(p) << std::endl;
  //   }
  // }

  //         name         rating  executible   data directory path
  std::tuple<std::string, double, std::string, std::string> get_team_info(bool is_red) const
  {
    const auto prefix = std::string("team") + (is_red ? "A" : "B");

    return std::make_tuple(getSelf()->getField(prefix + "Name")->getSFString(),
                           getSelf()->getField(prefix + "Rating")->getSFFloat(),
                           getSelf()->getField(prefix + "Executable")->getSFString(),
                           getSelf()->getField(prefix + "DataPath")->getSFString()
                           );
  }

  const unsigned char* get_image(bool is_red) const
  {
    return pc_cams_[is_red ? C_CAMA : C_CAMB]->getImage();
  }

  void reset_position()
  {
    namespace c = constants;

    const auto reset_node = [&](webots::Node* pn, double x, double y, double z, double th) {
      const double translation[] = {x, y, z};
      const double rotation[] = {0, 1, 0, th - c::PI / 2};
      pn->getField("translation")->setSFVec3f(translation);
      pn->getField("rotation")->setSFRotation(rotation);
      pn->resetPhysics();
    };

    reset_node(getFromDef(c::DEF_BALL), 0, c::BALL_RADIUS, 0, 0);

    for(const auto& is_red : {true, false}) {
      const auto s = is_red ? 1 : -1;
      for(std::size_t id = 0; id < 5; ++id) {
        reset_node(getFromDef(robot_name(is_red, id)),
                   c::ROBOT_INIT_POSTURE[id][0] * s,
                   c::ROBOT_SIZE / 2,
                   c::ROBOT_INIT_POSTURE[id][1] * s,
                   c::ROBOT_INIT_POSTURE[id][2] + (is_red ? 0. : c::PI));
      }
    }
  }

  std::array<double, 2> get_ball_position() const
  {
    webots::Node* pn_ball = getFromDef(constants::DEF_BALL);

    const double* position = pn_ball->getPosition();

    const double x = position[0];
    const double y = -position[2];

    return {x, y};
  }

  //         x       y       th
  std::tuple<double, double, double> get_robot_posture(bool is_red, std::size_t id) const
  {
    webots::Node* pn_robot = getFromDef(robot_name(is_red, id));

    const double* position = pn_robot->getPosition();
    const double* orientation = pn_robot->getOrientation();

    const double x = position[0];
    const double y = -position[2];
    const double th = std::atan2(orientation[2], orientation[8]) + constants::PI / 2;

    return std::make_tuple(x, y, th);
  }

  void send_to_foulzone(bool is_red, std::size_t id)
  {
    namespace c = constants;

    webots::Node* pn_robot = getFromDef(robot_name(is_red, id));

    const auto s = is_red ? 1 : -1;
    const double translation[] = {c::ROBOT_FOUL_POSTURE[id][0] * s,
                                  c::ROBOT_SIZE / 2,
                                  c::ROBOT_FOUL_POSTURE[id][1] * s};
    const double rotation[] = { 0, 1, 0,
                                c::ROBOT_FOUL_POSTURE[id][2] + (is_red ? 0. : c::PI) - c::PI / 2 };
    pn_robot->getField("translation")->setSFVec3f(translation);
    pn_robot->getField("rotation")->setSFRotation(rotation);
    pn_robot->getField("data")->setSFString("0 0");
    pn_robot->resetPhysics();
  }

  void set_linear_wheel_speed(bool is_red, std::size_t id, const std::array<double, 2>& speed)
  {
    namespace c = constants;

    webots::Node* pn_robot = getFromDef(robot_name(is_red, id));

    const auto left = std::max(std::min(speed[0], c::MAX_LINEAR_VELOCITY), -c::MAX_LINEAR_VELOCITY);
    const auto right = std::max(std::min(speed[1], c::MAX_LINEAR_VELOCITY), -c::MAX_LINEAR_VELOCITY);

    pn_robot->getField("data")->setSFString(std::to_string(left / c::WHEEL_RADIUS) + " " +
                                            std::to_string(right / c::WHEEL_RADIUS));
  }

private: // private member functions
  void remove_ball_and_robots()
  {
    const auto& remove_node = [&](const std::string& defname) {
      auto* const pn = getFromDef(defname);
      if(pn) {
        pn->remove();
      }
    };

    remove_node(constants::DEF_BALL);

    for(const auto& is_red : {true, false}) {
      for(std::size_t id = 0; id < 5; ++id) {
        remove_node(robot_name(is_red, id));
      }
    }
  }

  void place_ball_and_robots()
  {
    using namespace constants;

    auto* const pn_root = getRoot();
    assert(pn_root);
    auto* const pf_children = pn_root->getField("children");

    // put ball
    pf_children->importMFNodeFromString(-1, generate_ball_node_string(0, BALL_RADIUS, 0, BALL_RADIUS));

    // put robots
    {
      std::stringstream ss;
      for(const auto& is_red : {true, false}) {
        const auto s = is_red ? 1 : -1;
        for(std::size_t id = 0; id < 5; ++id) {
          const auto x  = ROBOT_INIT_POSTURE[id][0] * s;
          const auto y  = ROBOT_SIZE / 2;
          const auto z  = ROBOT_INIT_POSTURE[id][1] * s;
          const auto th = ROBOT_INIT_POSTURE[id][2] + (is_red ? 0. : constants::PI);
          ss << generate_robot_node_string(x, y, z, th, is_red, id);
        }
      }
      pf_children->importMFNodeFromString(-1, ss.str());
    }
  }

  void control_visibility()
  {
    using namespace constants;

    // DEF_GRASS is not visible to cam a and cam b, optional
    {
      auto* pn_grass_ = getFromDef(DEF_GRASS);
      if(pn_grass_) {
        pn_grass_->setVisibility(pn_cams_[N_CAMA], false);
        pn_grass_->setVisibility(pn_cams_[N_CAMB], false);
      }
    }

    // BALLSHAPE is visible only to viewpoint, ORANGESHAPE is to cam_a and cam_b, mandatory
    {
      auto* pn_ballshape = getFromDef(DEF_BALLSHAPE);
      auto* pn_orangeshape = getFromDef(DEF_ORANGESHAPE);
      if(!pn_ballshape || !pn_orangeshape) {
        throw std::runtime_error("No ball shape");
      }

      pn_ballshape->setVisibility(pn_cams_[N_CAMA], false);
      pn_ballshape->setVisibility(pn_cams_[N_CAMB], false);
      pn_orangeshape->setVisibility(pn_cams_[N_VIEWPOINT], false);
    }

    // Goalpost is visible only to viewpoint, optional
    {
      auto* pn_goalpost = getFromDef(DEF_GOALPOST);
      if(pn_goalpost) {
        pn_goalpost->setVisibility(pn_cams_[N_CAMA], false);
        pn_goalpost->setVisibility(pn_cams_[N_CAMB], false);
      }
    }

    // patches
    {
      for(const auto& pn_team : pn_robots_) {
        for(const auto& pn_robot : pn_team) {
          auto* pf_patches = pn_robot->getField("patches");

          assert(pf_patches && (pf_patches->getCount() == 3));

          auto* pn_number  = pf_patches->getMFNode(0);
          auto* pn_id_red  = pf_patches->getMFNode(1);
          auto* pn_id_blue = pf_patches->getMFNode(2);

          pn_number->setVisibility(pn_cams_[N_CAMA], false);
          pn_number->setVisibility(pn_cams_[N_CAMB], false);
          pn_id_red->setVisibility(pn_cams_[N_VIEWPOINT], false);
          pn_id_red->setVisibility(pn_cams_[N_CAMB], false);
          pn_id_blue->setVisibility(pn_cams_[N_VIEWPOINT], false);
          pn_id_blue->setVisibility(pn_cams_[N_CAMA], false);
        }
      }
    }
  }

  void enable_cameras(std::size_t period_in_ms)
  {
    for(auto* pc : pc_cams_) {
      pc->enable(period_in_ms);
    }
  }

private: // private member variables
  std::array<webots::Node*, 3> pn_cams_;
  std::array<webots::Camera*, 2> pc_cams_;

  webots::Node* pn_ball_;
  std::array<std::array<webots::Node*, 5>, 2> pn_robots_;
};

#endif // H_SUPERVISOR_HPP