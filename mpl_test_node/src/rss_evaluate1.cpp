#include "bag_reader.hpp"
#include <std_msgs/Bool.h>
#include <ros/ros.h>
#include <planning_ros_msgs/VoxelMap.h>
#include <ros_utils/data_ros_utils.h>
#include <ros_utils/primitive_ros_utils.h>
#include <primitive/poly_solver.h>
#include <mapping_utils/voxel_grid.h>
#include <planner/mp_map_util.h>
#include <fstream>

using namespace MPL;

std::unique_ptr<VoxelGrid> voxel_mapper_;
std::shared_ptr<MPL::VoxelMapUtil> map_util_;

Vec3i dim_;
Waypoint start_, goal_;
std_msgs::Header header;

MPMapUtil planner_ (false);
MPMapUtil replan_planner_ (false);

ros::Publisher map_pub;
ros::Publisher sg_pub;
ros::Publisher changed_prs_pub;
std::vector<ros::Publisher> prs_pub;
std::vector<ros::Publisher> traj_pub;
std::vector<ros::Publisher> linked_cloud_pub;
std::vector<ros::Publisher> close_cloud_pub;
std::vector<ros::Publisher> open_cloud_pub;
std::vector<ros::Publisher> expanded_cloud_pub;

//std::ofstream myfile;
int addition_num = 50;

void setMap(std::shared_ptr<MPL::VoxelMapUtil>& map_util, const planning_ros_msgs::VoxelMap& msg) {
  Vec3f ori(msg.origin.x, msg.origin.y, msg.origin.z);
  Vec3i dim(msg.dim.x, msg.dim.y, msg.dim.z);
  decimal_t res = msg.resolution;
  std::vector<signed char> map = msg.data;

  map_util->setMap(ori, dim, map, res);
}

void getMap(std::shared_ptr<MPL::VoxelMapUtil>& map_util, planning_ros_msgs::VoxelMap& map) {
  Vec3f ori = map_util->getOrigin();
  Vec3i dim = map_util->getDim();
  decimal_t res = map_util->getRes();

  map.origin.x = ori(0);
  map.origin.y = ori(1);
  map.origin.z = ori(2);

  map.dim.x = dim(0);
  map.dim.y = dim(1);
  map.dim.z = dim(2);
  map.resolution = res;

  map.data = map_util->getMap();
}

Vec3i generate_point() {
  static std::random_device rd;  //Will be used to obtain a seed for the random number engine
  static std::mt19937 gen(rd()); //Standard mersenne_twister_engine seeded with rd()
  std::uniform_int_distribution<int> dis_x(0, dim_(0)-1);
  std::uniform_int_distribution<int> dis_y(0, dim_(1)-1);
  std::uniform_int_distribution<int> dis_z(0, dim_(2)-1);

  return Vec3i(dis_x(gen), dis_y(gen), dis_z(gen));
}


void visualizeGraph(int id, const MPMapUtil& planner) {
  if(id < 0 || id > 1)
    return;

  //Publish location of start and goal
  sensor_msgs::PointCloud sg_cloud;
  sg_cloud.header = header;
  geometry_msgs::Point32 pt1, pt2;
  pt1.x = start_.pos(0), pt1.y = start_.pos(1), pt1.z = start_.pos(2);
  pt2.x = goal_.pos(0), pt2.y = goal_.pos(1), pt2.z = goal_.pos(2);
  sg_cloud.points.push_back(pt1);
  sg_cloud.points.push_back(pt2); 
  sg_pub.publish(sg_cloud);


  //Publish expanded nodes
  sensor_msgs::PointCloud expanded_ps = vec_to_cloud(planner.getExpandedNodes());
  expanded_ps.header = header;
  expanded_cloud_pub[id].publish(expanded_ps);

  //Publish nodes in closed set
  sensor_msgs::PointCloud close_ps = vec_to_cloud(planner.getCloseSet());
  close_ps.header = header;
  close_cloud_pub[id].publish(close_ps);

  //Publish nodes in open set
  sensor_msgs::PointCloud open_ps = vec_to_cloud(planner.getOpenSet());
  open_ps.header = header;
  open_cloud_pub[id].publish(open_ps);

  //Publish nodes in open set
  sensor_msgs::PointCloud linked_ps = vec_to_cloud(planner.getLinkedNodes());
  linked_ps.header = header;
  linked_cloud_pub[id].publish(linked_ps);

  //Publish primitives
  planning_ros_msgs::Primitives prs_msg = toPrimitivesROSMsg(planner.getAllPrimitives());
  //planning_ros_msgs::Primitives prs_msg = toPrimitivesROSMsg(planner.getValidPrimitives());
  prs_msg.header =  header;
  prs_pub[id].publish(prs_msg);
}

void plan(double density = 0) {
  static bool terminate = false;
  if(terminate)
    return;

  ros::Time t0 = ros::Time::now();
  bool valid = planner_.plan(start_, goal_);
  if(!valid) {
    ROS_ERROR("Failed! Takes %f sec for planning", (ros::Time::now() - t0).toSec());
    terminate = true;
  }
  else{

    ROS_WARN("Succeed! Takes %f sec for normal planning, openset: [%zu], closeset (expanded): [%zu](%zu), total: [%zu]", 
        (ros::Time::now() - t0).toSec(), planner_.getOpenSet().size(), planner_.getCloseSet().size(), 
        planner_.getExpandedNodes().size(),
        planner_.getOpenSet().size() + planner_.getCloseSet().size());

    //Publish trajectory
    Trajectory traj = planner_.getTraj();
    planning_ros_msgs::Trajectory traj_msg = toTrajectoryROSMsg(traj);
    traj_msg.header = header;
    traj_pub[0].publish(traj_msg);
    //printf("================== Traj -- J(0): %f, J(1): %f, J(2): %f, total time: %f\n", traj.J(0), traj.J(1), traj.J(2), traj.getTotalTime());
  }

  visualizeGraph(0, planner_);

  t0 = ros::Time::now();
  valid = replan_planner_.plan(start_, goal_);
  if(!valid) {
    ROS_ERROR("Failed! Takes %f sec for planning", (ros::Time::now() - t0).toSec());
  }
  else{
    ROS_WARN("Succeed! Takes %f sec for LPA* planning, openset: [%zu], closeset (expanded): [%zu](%zu), total: [%zu]", 
        (ros::Time::now() - t0).toSec(), replan_planner_.getOpenSet().size(), replan_planner_.getCloseSet().size(), 
        replan_planner_.getExpandedNodes().size(),
        replan_planner_.getOpenSet().size() + replan_planner_.getCloseSet().size());

    //Publish trajectory
    Trajectory traj = replan_planner_.getTraj();
    planning_ros_msgs::Trajectory traj_msg = toTrajectoryROSMsg(traj);
    traj_msg.header = header;
    traj_pub[1].publish(traj_msg);
    //printf("================== Traj -- J(0): %f, J(1): %f, J(2): %f, total time: %f\n", traj.J(0), traj.J(1), traj.J(2), traj.getTotalTime());
  }

  visualizeGraph(1, replan_planner_);
  //myfile << addition_num, density, 
  printf(ANSI_COLOR_CYAN "==========================================\n\n" ANSI_COLOR_RESET);
}


void replanCallback(const std_msgs::Bool::ConstPtr& msg) {
  int cnt = 0;
  vec_Vec3i new_obs;
  while(cnt < addition_num) {
    Vec3i pn = generate_point();
    if(map_util_->isFree(pn)) {
      const Vec3f pt = map_util_->intToFloat(pn);
      if((pt - start_.pos).topRows(2).norm() < 0.5 ||
          (pt - goal_.pos).topRows(2).norm() < 0.5)
        continue;
      for(int i = 0; i < dim_(2); i++)
        new_obs.push_back(Vec3i(pn(0), pn(1), i));
      voxel_mapper_->fill(pn(0), pn(1));
      cnt ++;
    }
  }

  planning_ros_msgs::VoxelMap map = voxel_mapper_->getMap();
  setMap(map_util_, map);
  //Publish the dilated map for visualization
  map_util_->freeUnKnown();
  getMap(map_util_, map);
  map.header = header;
  map_pub.publish(map);

  if(replan_planner_.initialized()) {
    planning_ros_msgs::Primitives prs_msg = toPrimitivesROSMsg(replan_planner_.updateBlockedNodes(new_obs));
    prs_msg.header.frame_id = "map";
    changed_prs_pub.publish(prs_msg);
  }
 

  static int obs_number = 0;
  obs_number += cnt;
  printf("Density: %f\n", (float) obs_number / (dim_(0) * dim_(1)));
  plan();
}


int main(int argc, char ** argv){
  ros::init(argc, argv, "test");
  ros::NodeHandle nh("~");

  map_pub = nh.advertise<planning_ros_msgs::VoxelMap>("voxel_map", 1, true);
  sg_pub = nh.advertise<sensor_msgs::PointCloud>("start_and_goal", 1, true);

  ros::Publisher prs_pub0 = nh.advertise<planning_ros_msgs::Primitives>("primitives0", 1, true);
  ros::Publisher prs_pub1 = nh.advertise<planning_ros_msgs::Primitives>("primitives1", 1, true);
  prs_pub.push_back(prs_pub0), prs_pub.push_back(prs_pub1);

  ros::Publisher traj_pub0 = nh.advertise<planning_ros_msgs::Trajectory>("trajectory0", 1, true);
  ros::Publisher traj_pub1 = nh.advertise<planning_ros_msgs::Trajectory>("trajectory1", 1, true);
  traj_pub.push_back(traj_pub0), traj_pub.push_back(traj_pub1);

  ros::Publisher close_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("close_cloud0", 1, true);
  ros::Publisher close_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("close_cloud1", 1, true);
  close_cloud_pub.push_back(close_cloud_pub0), close_cloud_pub.push_back(close_cloud_pub1);

  ros::Publisher open_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("open_set0", 1, true);
  ros::Publisher open_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("open_set1", 1, true);
  open_cloud_pub.push_back(open_cloud_pub0), open_cloud_pub.push_back(open_cloud_pub1);

  ros::Publisher linked_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("linked_pts0", 1, true);
  ros::Publisher linked_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("linked_pts1", 1, true);
  linked_cloud_pub.push_back(linked_cloud_pub0), linked_cloud_pub.push_back(linked_cloud_pub1);

  ros::Publisher expanded_cloud_pub0 = nh.advertise<sensor_msgs::PointCloud>("expanded_cloud0", 1, true);
  ros::Publisher expanded_cloud_pub1 = nh.advertise<sensor_msgs::PointCloud>("expanded_cloud1", 1, true);
  expanded_cloud_pub.push_back(expanded_cloud_pub0), expanded_cloud_pub.push_back(expanded_cloud_pub1);

  ros::Subscriber replan_sub = nh.subscribe("replan", 1, replanCallback);
  changed_prs_pub = nh.advertise<planning_ros_msgs::Primitives>("changed_primitives", 1, true);

  header.frame_id = std::string("map");

  Vec3f ori, dim;
  nh.param("origin_x", ori(0), 0.0);
  nh.param("origin_y", ori(1), 2.5);
  nh.param("origin_z", ori(2), 0.0);

  nh.param("range_x", dim(0), 10.0);
  nh.param("range_y", dim(1), 5.0);
  nh.param("range_z", dim(2), 1.0);

  double res;
  nh.param("resolution", res, 0.1);

  dim_(0) = dim(0) / res;
  dim_(1) = dim(1) / res;
  dim_(2) = dim(2) / res;

  voxel_mapper_.reset(new VoxelGrid(ori, dim, res));
  //Initialize map util 
  map_util_.reset(new MPL::VoxelMapUtil);

  planning_ros_msgs::VoxelMap map = voxel_mapper_->getMap();
  setMap(map_util_, map);
  map_util_->freeUnKnown();
  getMap(map_util_, map);
  map.header = header;
  map_pub.publish(map);



  //Set start and goal
  double start_x, start_y, start_z;
  nh.param("start_x", start_x, 12.5);
  nh.param("start_y", start_y, 1.4);
  nh.param("start_z", start_z, 0.0);
  double start_vx, start_vy, start_vz;
  nh.param("start_vx", start_vx, 0.0);
  nh.param("start_vy", start_vy, 0.0);
  nh.param("start_vz", start_vz, 0.0);
  double goal_x, goal_y, goal_z;
  nh.param("goal_x", goal_x, 6.4);
  nh.param("goal_y", goal_y, 16.6);
  nh.param("goal_z", goal_z, 0.0);
 
  start_.pos = Vec3f(start_x, start_y, start_z);
  start_.vel = Vec3f(start_vx, start_vy, start_vz);
  start_.acc = Vec3f(0, 0, 0);
  start_.use_pos = true;
  start_.use_vel = true;
  start_.use_acc = true;
  start_.use_jrk = false;

  goal_.pos = Vec3f(goal_x, goal_y, goal_z);
  goal_.vel = Vec3f(0, 0, 0);
  goal_.acc = Vec3f(0, 0, 0);
  goal_.use_pos = start_.use_pos;
  goal_.use_vel = start_.use_vel;
  goal_.use_acc = start_.use_acc;
  goal_.use_jrk = start_.use_jrk;


  //Initialize planner
  double dt, v_max, a_max, j_max, u_max;
  int max_num, ndt;
  bool use_3d;
  nh.param("dt", dt, 1.0);
  nh.param("ndt", ndt, -1);
  nh.param("v_max", v_max, 2.0);
  nh.param("a_max", a_max, 1.0);
  nh.param("j_max", j_max, 1.0);
  nh.param("u_max", u_max, 1.0);
  nh.param("max_num", max_num, -1);
  nh.param("use_3d", use_3d, false);


  planner_.setMapUtil(map_util_); // Set collision checking function
  planner_.setEpsilon(1.0); // Set greedy param (default equal to 1)
  planner_.setVmax(v_max); // Set max velocity
  planner_.setAmax(a_max); // Set max acceleration
  planner_.setJmax(j_max); // Set jrk (as control input)
  planner_.setUmax(u_max);// 2D discretization with 1
  planner_.setDt(dt); // Set dt for each primitive
  planner_.setTmax(ndt * dt); // Set dt for each primitive
  planner_.setMaxNum(max_num); // Set maximum allowed expansion, -1 means no limitation
  planner_.setU(1, false);// 2D discretization with 1
  planner_.setTol(0.2, 1, 1); // Tolerance for goal region
  planner_.setLPAstar(false); // Use Astar


  replan_planner_.setMapUtil(map_util_); // Set collision checking function
  replan_planner_.setEpsilon(1.0); // Set greedy param (default equal to 1)
  replan_planner_.setVmax(v_max); // Set max velocity
  replan_planner_.setAmax(a_max); // Set max acceleration (as control input)
  replan_planner_.setJmax(j_max); // Set jrk (as control input)
  replan_planner_.setUmax(u_max);// 2D discretization with 1
  replan_planner_.setDt(dt); // Set dt for each primitive
  replan_planner_.setTmax(ndt * dt); // Set dt for each primitive
  replan_planner_.setMaxNum(-1); // Set maximum allowed expansion, -1 means no limitation
  replan_planner_.setU(1, false);// 2D discretization with 1
  replan_planner_.setTol(0.2, 1, 1); // Tolerance for goal region
  replan_planner_.setLPAstar(true); // Use LPAstar


  //myfile.open("record-"+std::to_string(addition_num)+".csv");
  //myfile << "Addition Num, Obstacle Density, NormalAstar Time, LAstar Time, NormalAstar Expansion, LPAstar Expansion\n";

  plan();

  
  //myfile.close();
  ros::spin();

  return 0;
}