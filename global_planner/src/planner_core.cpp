/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, 2013, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Eitan Marder-Eppstein
 *         David V. Lu!!
 *********************************************************************/
#include <global_planner/planner_core.h>
#include <pluginlib/class_list_macros.h>
#include <tf/transform_listener.h>
#include <costmap_2d/cost_values.h>
#include <costmap_2d/costmap_2d.h>

#include <global_planner/dijkstra.h>
#include <global_planner/astar.h>
#include <global_planner/grid_path.h>
#include <global_planner/gradient_path.h>
#include <global_planner/quadratic_calculator.h>

#include <vector>
#include <map>

using namespace std;

//register this planner as a BaseGlobalPlanner plugin
PLUGINLIB_EXPORT_CLASS(global_planner::GlobalPlanner, nav_core::BaseGlobalPlanner)

namespace global_planner {

void GlobalPlanner::outlineMap(unsigned char* costarr, int nx, int ny, unsigned char value) {
    unsigned char* pc = costarr;
    for (int i = 0; i < nx; i++)
        *pc++ = value;
    pc = costarr + (ny - 1) * nx;
    for (int i = 0; i < nx; i++)
        *pc++ = value;
    pc = costarr;
    for (int i = 0; i < ny; i++, pc += nx)
        *pc = value;
    pc = costarr + nx - 1;
    for (int i = 0; i < ny; i++, pc += nx)
        *pc = value;
}

GlobalPlanner::GlobalPlanner() :
        costmap_(NULL), initialized_(false), allow_unknown_(true) {
}

GlobalPlanner::GlobalPlanner(std::string name, costmap_2d::Costmap2D* costmap, std::string frame_id) :
        costmap_(NULL), initialized_(false), allow_unknown_(true) {
    //initialize the planner
    initialize(name, costmap, frame_id);
}

GlobalPlanner::~GlobalPlanner() {
    if (p_calc_)
        delete p_calc_;
    if (planner_)
        delete planner_;
    if (path_maker_)
        delete path_maker_;
    if (dsrv_)
        delete dsrv_;
}

void GlobalPlanner::initialize(std::string name, costmap_2d::Costmap2DROS* costmap_ros) {
    initialize(name, costmap_ros->getCostmap(), costmap_ros->getGlobalFrameID());
}

void GlobalPlanner::initialize(std::string name, costmap_2d::Costmap2D* costmap, std::string frame_id) {
    if (!initialized_) {
        ros::NodeHandle private_nh("~/" + name);
        costmap_ = costmap;
        frame_id_ = frame_id;

        unsigned int cx = costmap->getSizeInCellsX(), cy = costmap->getSizeInCellsY();

        private_nh.param("old_navfn_behavior", old_navfn_behavior_, false);
        if(!old_navfn_behavior_)
            convert_offset_ = 0.5;
        else
            convert_offset_ = 0.0;

        bool use_quadratic;
        private_nh.param("use_quadratic", use_quadratic, true);
        if (use_quadratic)
            p_calc_ = new QuadraticCalculator(cx, cy);
        else
            p_calc_ = new PotentialCalculator(cx, cy);

        bool use_dijkstra;
        private_nh.param("use_dijkstra", use_dijkstra, true);
        if (use_dijkstra)
        {
            DijkstraExpansion* de = new DijkstraExpansion(p_calc_, cx, cy);
            if(!old_navfn_behavior_)
                de->setPreciseStart(true);
            planner_ = de;
        }
        else
            planner_ = new AStarExpansion(p_calc_, cx, cy);

        bool use_grid_path;
        private_nh.param("use_grid_path", use_grid_path, false);

        if (use_grid_path){
            path_maker_ = new GridPath(p_calc_);
            path_maker_backup_ = NULL;
        }
        else{
            path_maker_ = new GradientPath(p_calc_);
            path_maker_backup_ = new GridPath(p_calc_);
            //we use the grid path as backup as sometimes the gradient path
            //has issues extracting paths
        }

            
        orientation_filter_ = new OrientationFilter();

        plan_pub_ = private_nh.advertise<nav_msgs::Path>("plan", 1);
        potential_pub_ = private_nh.advertise<nav_msgs::OccupancyGrid>("potential", 1);

        private_nh.param("allow_unknown", allow_unknown_, true);
        planner_->setHasUnknown(allow_unknown_);
        private_nh.param("planner_window_x", planner_window_x_, 0.0);
        private_nh.param("planner_window_y", planner_window_y_, 0.0);
        private_nh.param("default_tolerance", default_tolerance_, 0.0);
        private_nh.param("goal_obstacle_clearance", goal_obstacle_clearance_, 0.0);
        private_nh.param("publish_scale", publish_scale_, 100);

        double costmap_pub_freq;
        private_nh.param("planner_costmap_publish_frequency", costmap_pub_freq, 0.0);

        //get the tf prefix
        ros::NodeHandle prefix_nh;
        tf_prefix_ = tf::getPrefixParam(prefix_nh);

        make_plan_srv_ = private_nh.advertiseService("make_plan", &GlobalPlanner::makePlanService, this);

        dsrv_ = new dynamic_reconfigure::Server<global_planner::GlobalPlannerConfig>(ros::NodeHandle("~/" + name));
        dynamic_reconfigure::Server<global_planner::GlobalPlannerConfig>::CallbackType cb = boost::bind(
                &GlobalPlanner::reconfigureCB, this, _1, _2);
        dsrv_->setCallback(cb);

        initialized_ = true;
    } else
        ROS_WARN("This planner has already been initialized, you can't call it twice, doing nothing");

}

void GlobalPlanner::reconfigureCB(global_planner::GlobalPlannerConfig& config, uint32_t level) {
    planner_->setLethalCost(config.lethal_cost);
    path_maker_->setLethalCost(config.lethal_cost);
    if(path_maker_backup_){
      path_maker_backup_->setLethalCost(config.lethal_cost);
    }
    planner_->setNeutralCost(config.neutral_cost);
    planner_->setFactor(config.cost_factor);
    publish_potential_ = config.publish_potential;
    orientation_filter_->setMode(config.orientation_mode);
}

void GlobalPlanner::clearRobotCell(const tf::Stamped<tf::Pose>& global_pose, unsigned int mx, unsigned int my) {
    if (!initialized_) {
        ROS_ERROR(
                "This planner has not been initialized yet, but it is being used, please call initialize() before use");
        return;
    }

    //set the associated costs in the cost map to be free
    costmap_->setCost(mx, my, costmap_2d::FREE_SPACE);
}

bool GlobalPlanner::makePlanService(nav_msgs::GetPlan::Request& req, nav_msgs::GetPlan::Response& resp) {
    makePlan(req.start, req.goal, resp.plan.poses);

    resp.plan.header.stamp = ros::Time::now();
    resp.plan.header.frame_id = frame_id_;

    return true;
}

void GlobalPlanner::mapToWorld(double mx, double my, double& wx, double& wy) {
    wx = costmap_->getOriginX() + (mx+convert_offset_) * costmap_->getResolution();
    wy = costmap_->getOriginY() + (my+convert_offset_) * costmap_->getResolution();
}

bool GlobalPlanner::worldToMap(double wx, double wy, double& mx, double& my) {
    double origin_x = costmap_->getOriginX(), origin_y = costmap_->getOriginY();
    double resolution = costmap_->getResolution();

    if (wx < origin_x || wy < origin_y)
        return false;

    mx = (wx - origin_x) / resolution - convert_offset_;
    my = (wy - origin_y) / resolution - convert_offset_;

    if (mx < costmap_->getSizeInCellsX() && my < costmap_->getSizeInCellsY())
        return true;

    return false;
}

bool GlobalPlanner::makePlan(const geometry_msgs::PoseStamped& start, const geometry_msgs::PoseStamped& goal,
                           std::vector<geometry_msgs::PoseStamped>& plan) {
    return makePlan(start, goal, default_tolerance_, plan);
}


  bool GlobalPlanner::isPointOccupied(unsigned char* costs, double goal_x, double goal_y, int nx, int ny){
     int goal_index = getMapIndex(goal_x, goal_y, nx);

     float c = getCost(costs, goal_index);

     double lethal_cost_ = planner_->getLethalCost();
     if(c == lethal_cost_){
       return true;
     }
     else
       return false; //do we want to check here if its in unknown space??
  }

  bool GlobalPlanner::findClearGoal(unsigned char* costs, double goal_x, double goal_y, double &new_goal_x, double &new_goal_y, double xy_tolerance, int nx, int ny){
    //do a local search in the neighborhood - and find the x,y location with the lowest obstacle cost 
    int goal_index = getMapIndex(goal_x, goal_y, nx);

    float min_cost = getCost(costs, goal_index);

    bool found = false; 

    new_goal_x = goal_x; 
    new_goal_y = goal_y; 
    
    int x_t = xy_tolerance;
    int y_t = xy_tolerance; 

    for(int dx = -x_t; dx <= x_t; dx++){
      for(int dy = -y_t; dy <= y_t; dy++){
        if(dx == 0 && dy == 0){
          continue;
        }

        //Note : No guarantee that it will not go through a wall if set a high enough value (DON'T SET high value :)
        if(hypot(dx, dy) < xy_tolerance){
          double n_goal_x = goal_x + dx;
          double n_goal_y = goal_y + dy;

          if(n_goal_x >= nx || n_goal_x < 0)
            continue;
          if(n_goal_y >= ny || n_goal_y < 0)
            continue;

          int index = getMapIndex(n_goal_x, n_goal_y, nx);

          float cost = getCost(costs, index);

          if(cost < min_cost){
            min_cost= cost;
            new_goal_x = n_goal_x;
            new_goal_y = n_goal_y;
            found = true;
          }
        }
      }
    }
    return found; 
  }

  bool GlobalPlanner::findClosestOpenGoal(unsigned char* costs, double goal_x, double goal_y, double &new_goal_x, double &new_goal_y, double xy_tolerance, int nx, int ny){

    int goal_index = getMapIndex(goal_x, goal_y, nx);

    float c = getCost(costs, goal_index);

    double lethal_cost_ = planner_->getLethalCost();
    if(c == lethal_cost_){
      ROS_INFO("Original goal is in collision - checking neighbors => Tollerance : %f", xy_tolerance);

      int x_t = xy_tolerance;
      int y_t = xy_tolerance;

      bool found = false;

      double n_end_x, n_end_y;
      float min_cost = lethal_cost_;

      for(int dx = 0; dx < x_t; dx++){
        if(found)
          break;
        for(int dy = 0; dy < y_t; dy++){
          if(found)
            break;
          if(dx == 0 && dy == 0){
            continue;
          }

          vector<pair<double, double> > points;
          if(hypot(dx, dy) < xy_tolerance){
            points.push_back(make_pair(goal_x + dx, goal_y + dy));
            points.push_back(make_pair(goal_x + dx, goal_y - dy));
            points.push_back(make_pair(goal_x - dx, goal_y - dy));
            points.push_back(make_pair(goal_x - dx, goal_y + dy));

            for(int i=0; i < points.size(); i++){
              if(points[i].first >= nx || points[i].first < 0)
                continue;
              if(points[i].second >= ny || points[i].second < 0)
                continue;
              int index = getMapIndex(points[i].first, points[i].second, nx);

              float cost = getCost(costs, index);

              if(cost < lethal_cost_){
                found = true;
                n_end_x = points[i].first;
                n_end_y = points[i].second;
                min_cost = cost;
                break;
              }
            }
          }
        }
      }

      if(found){
        ROS_INFO("Found close neighbour with low cost : %f, %f, min_cost %f", n_end_x, n_end_y, min_cost);
        // set up start cell
        goal_index = getMapIndex(n_end_x, n_end_y, nx);
        new_goal_x = n_end_x;
        new_goal_y = n_end_y;
        return true;
      }
      else{
        new_goal_x = goal_x;
        new_goal_y = goal_y;
        return false;
      }
    }

    else{
      new_goal_x = goal_x;
      new_goal_y = goal_y;
      return true;
    }
  }

bool GlobalPlanner::makePlan(const geometry_msgs::PoseStamped& start, const geometry_msgs::PoseStamped& goal,
                           double tolerance, std::vector<geometry_msgs::PoseStamped>& plan) {
    boost::mutex::scoped_lock lock(mutex_);
    if (!initialized_) {
        ROS_ERROR(
                "This planner has not been initialized yet, but it is being used, please call initialize() before use");
        return false;
    }

    //clear the plan, just in case
    plan.clear();

    ros::NodeHandle n;
    std::string global_frame = frame_id_;

    //until tf can handle transforming things that are way in the past... we'll require the goal to be in our global frame
    if (tf::resolve(tf_prefix_, goal.header.frame_id) != tf::resolve(tf_prefix_, global_frame)) {
        ROS_ERROR(
                "The goal pose passed to this planner must be in the %s frame.  It is instead in the %s frame.", tf::resolve(tf_prefix_, global_frame).c_str(), tf::resolve(tf_prefix_, goal.header.frame_id).c_str());
        return false;
    }

    if (tf::resolve(tf_prefix_, start.header.frame_id) != tf::resolve(tf_prefix_, global_frame)) {
        ROS_ERROR(
                "The start pose passed to this planner must be in the %s frame.  It is instead in the %s frame.", tf::resolve(tf_prefix_, global_frame).c_str(), tf::resolve(tf_prefix_, start.header.frame_id).c_str());
        return false;
    }

    double wx = start.pose.position.x;
    double wy = start.pose.position.y;

    unsigned int start_x_i, start_y_i, goal_x_i, goal_y_i;
    double start_x, start_y, goal_x, goal_y;

    if (!costmap_->worldToMap(wx, wy, start_x_i, start_y_i)) {
        ROS_WARN(
                "The robot's start position is off the global costmap. Planning will always fail, are you sure the robot has been properly localized?");
        return false;
    }
    if(old_navfn_behavior_){
        start_x = start_x_i;
        start_y = start_y_i;
    }else{
        worldToMap(wx, wy, start_x, start_y);
    }

    wx = goal.pose.position.x;
    wy = goal.pose.position.y;

    if (!costmap_->worldToMap(wx, wy, goal_x_i, goal_y_i)) {
        ROS_WARN_THROTTLE(1.0,
                "The goal sent to the global planner is off the global costmap. Planning will always fail to this goal.");
        return false;
    }
    if(old_navfn_behavior_){
        goal_x = goal_x_i;
        goal_y = goal_y_i;
    }else{
        worldToMap(wx, wy, goal_x, goal_y);
    }

    //clear the starting cell within the costmap because we know it can't be an obstacle
    tf::Stamped<tf::Pose> start_pose;
    tf::poseStampedMsgToTF(start, start_pose);
    clearRobotCell(start_pose, start_x_i, start_y_i);

    int nx = costmap_->getSizeInCellsX(), ny = costmap_->getSizeInCellsY();

    //make sure to resize the underlying array that Navfn uses
    p_calc_->setSize(nx, ny);
    planner_->setSize(nx, ny);
    path_maker_->setSize(nx, ny);
    if(path_maker_backup_){
      path_maker_backup_->setSize(nx, ny);
    }
    potential_array_ = new float[nx * ny];

    outlineMap(costmap_->getCharMap(), nx, ny, costmap_2d::LETHAL_OBSTACLE);

    double tolerance_world = tolerance; 
    double tolerance_map = tolerance_world / costmap_->getResolution();

    bool original_goal_valid = true;
           
    //we can check if the map has a close point within tolerance that we can plan to 
    
    bool orig_goal_occupied = isPointOccupied(costmap_->getCharMap(), goal_x, goal_y, nx, ny);    

    if(orig_goal_occupied && tolerance_map >= 1){
      ROS_WARN("Checking if there is a close node to the goal");
      // set up start cell
      double new_goal_x, new_goal_y; 
      
      bool found_goal = findClosestOpenGoal(costmap_->getCharMap(), goal_x, goal_y, new_goal_x, new_goal_y, tolerance_map, nx, ny);

      if(found_goal){
        if(new_goal_x != goal_x || new_goal_y != goal_y){
          ROS_WARN("Found close goal");
          original_goal_valid = false;
          goal_x = new_goal_x;
          goal_y = new_goal_y;
        }
      }
    }

    if(goal_obstacle_clearance_ > 0){
      double tolerance_world_clear = goal_obstacle_clearance_;
      double tolerance_map_clear = tolerance_world_clear / costmap_->getResolution();
      double new_goal_x, new_goal_y; 
      if(findClearGoal(costmap_->getCharMap(), goal_x, goal_y, new_goal_x, new_goal_y, tolerance_map_clear, nx, ny)){
        ROS_DEBUG("Found Clear Goal => Original Goal : %f,%f -> New Goal : %f, %f\n", goal_x, goal_y, new_goal_x, new_goal_y);
        original_goal_valid = false; //so that it wont push the last point 
        goal_x = new_goal_x; 
        goal_y = new_goal_y;
      }
    }

    //we should always check if the goal is not reachable - will save us some computation 
    bool goal_occupied =  isPointOccupied(costmap_->getCharMap(), goal_x, goal_y, nx, ny);    
    
    if(goal_occupied){
      ROS_ERROR("Goal location is occupied - unable to find plan");
    }

    bool found_legal = false; 

    //Goal is in free space - lets see if we can get there (otherwise no point in planning)
    if(!goal_occupied){
      found_legal = planner_->calculatePotentials(costmap_->getCharMap(), start_x, start_y, goal_x, goal_y, nx * ny * 2, potential_array_);
    }

    if(found_legal){
      ROS_DEBUG("Global planner found path to goal");
    }

    if(!old_navfn_behavior_)
        planner_->clearEndpoint(costmap_->getCharMap(), potential_array_, goal_x_i, goal_y_i, 2);
    if(publish_potential_)
        publishPotential(potential_array_);

    if (found_legal) {
        //extract the plan
        if (getPlanFromPotential(start_x, start_y, goal_x, goal_y, goal, plan)) {
	  //make sure the goal we push on has the same timestamp as the rest of the plan
	  if(original_goal_valid){
            geometry_msgs::PoseStamped goal_copy = goal;
            goal_copy.header.stamp = ros::Time::now();
	    plan.pop_back();
            plan.push_back(goal_copy);
	  }
	  else{
	    geometry_msgs::PoseStamped &last_goal = plan[plan.size() - 1];
	    //override the orientation of the last goal 
	    last_goal.pose.orientation.x = goal.pose.orientation.x;
	    last_goal.pose.orientation.y = goal.pose.orientation.y;
	    last_goal.pose.orientation.z = goal.pose.orientation.z;
	    last_goal.pose.orientation.w = goal.pose.orientation.w;
	  }
        } else {
            ROS_ERROR("Failed to get a plan from potential when a legal potential was found. This shouldn't happen.");
        }
    }else{
        ROS_ERROR("Failed to get a plan.");
    }

    // add orientations if needed
    orientation_filter_->processPath(start, plan);
    
    //publish the plan for visualization purposes
    publishPlan(plan);
    delete potential_array_;
    return !plan.empty();
}

void GlobalPlanner::publishPlan(const std::vector<geometry_msgs::PoseStamped>& path) {
    if (!initialized_) {
        ROS_ERROR(
                "This planner has not been initialized yet, but it is being used, please call initialize() before use");
        return;
    }

    //create a message for the plan
    nav_msgs::Path gui_path;
    gui_path.poses.resize(path.size());

    if (!path.empty()) {
        gui_path.header.frame_id = path[0].header.frame_id;
        gui_path.header.stamp = path[0].header.stamp;
    }

    // Extract the plan in world co-ordinates, we assume the path is all in the same frame
    for (unsigned int i = 0; i < path.size(); i++) {
        gui_path.poses[i] = path[i];
    }

    plan_pub_.publish(gui_path);
}

bool GlobalPlanner::getPlanFromPotential(double start_x, double start_y, double goal_x, double goal_y,
                                      const geometry_msgs::PoseStamped& goal,
                                       std::vector<geometry_msgs::PoseStamped>& plan) {
    if (!initialized_) {
        ROS_ERROR(
                "This planner has not been initialized yet, but it is being used, please call initialize() before use");
        return false;
    }

    std::string global_frame = frame_id_;

    //clear the plan, just in case
    plan.clear();

    std::vector<std::pair<float, float> > path;

    if (!path_maker_->getPath(potential_array_, start_x, start_y, goal_x, goal_y, path)) {
      bool backup_failed = false; 
      if(path_maker_backup_){
	ROS_WARN("Path extraction failed - trying backup grid extractor"); 
	backup_failed = !path_maker_backup_->getPath(potential_array_, start_x, start_y, goal_x, goal_y, path); 
      }
      if(backup_failed){
	ROS_ERROR("NO PATH!");
	return false;
      }
      else{
	ROS_WARN("Backup extractor worked\n");
      }
    }

    ros::Time plan_time = ros::Time::now();
    for (int i = path.size() -1; i>=0; i--) {
        std::pair<float, float> point = path[i];
        //convert the plan to world coordinates
        double world_x, world_y;
        mapToWorld(point.first, point.second, world_x, world_y);

        geometry_msgs::PoseStamped pose;
        pose.header.stamp = plan_time;
        pose.header.frame_id = global_frame;
        pose.pose.position.x = world_x;
        pose.pose.position.y = world_y;
        pose.pose.position.z = 0.0;
        pose.pose.orientation.x = 0.0;
        pose.pose.orientation.y = 0.0;
        pose.pose.orientation.z = 0.0;
        pose.pose.orientation.w = 1.0;
        plan.push_back(pose);
    }
    if(old_navfn_behavior_){
            plan.push_back(goal);
    }
    return !plan.empty();
}

void GlobalPlanner::publishPotential(float* potential)
{
    int nx = costmap_->getSizeInCellsX(), ny = costmap_->getSizeInCellsY();
    double resolution = costmap_->getResolution();
    nav_msgs::OccupancyGrid grid;
    // Publish Whole Grid
    grid.header.frame_id = frame_id_;
    grid.header.stamp = ros::Time::now();
    grid.info.resolution = resolution;

    grid.info.width = nx;
    grid.info.height = ny;

    double wx, wy;
    costmap_->mapToWorld(0, 0, wx, wy);
    grid.info.origin.position.x = wx - resolution / 2;
    grid.info.origin.position.y = wy - resolution / 2;
    grid.info.origin.position.z = 0.0;
    grid.info.origin.orientation.w = 1.0;

    grid.data.resize(nx * ny);

    float max = 0.0;
    for (unsigned int i = 0; i < grid.data.size(); i++) {
        float potential = potential_array_[i];
        if (potential < POT_HIGH) {
            if (potential > max) {
                max = potential;
            }
        }
    }

    for (unsigned int i = 0; i < grid.data.size(); i++) {
        if (potential_array_[i] >= POT_HIGH) {
            grid.data[i] = -1;
        } else
            grid.data[i] = potential_array_[i] * publish_scale_ / max;
    }
    potential_pub_.publish(grid);
}

} //end namespace global_planner

