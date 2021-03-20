//无人机追踪目标（此处先直接用目标位置进行追踪）
//无人机吊舱根据视觉算法的反馈调节角度，保证目标在相机中心位置
//ROS 头文件
#include <ros/ros.h>
#include <iostream>
#include "mission_utils.h"
#include "gimbal_control.h"
#include "message_utils.h"

using namespace std;
using namespace Eigen;

#define NODE_NAME "gimbal_control_demo"
#define PI 3.1415926

bool hold_mode;
bool ignore_vision;
Eigen::Vector3d gimbal_att_sp;
Eigen::Vector3d gimbal_att;
Eigen::Vector3d gimbal_att_deg;
Eigen::Vector3d gimbal_att_rate;
Eigen::Vector3d gimbal_att_rate_deg;
nav_msgs::Odometry GroundTruth;             // 降落板真实位置（仿真中由Gazebo插件提供）
Detection_result landpad_det;               // 检测结果
float sight_angle[2];

float kp_track[3];         //控制参数 - 比例参数
float kpyaw_track;
float start_point[3];    // 起始降落位置
Eigen::Vector3d roi_point;
//---------------------------------------Drone---------------------------------------------
prometheus_msgs::DroneState _DroneState;    // 无人机状态
Eigen::Matrix3f R_Body_to_ENU,R_camera_to_body;              // 无人机机体系至惯性系转换矩阵

prometheus_msgs::ControlCommand Command_Now;                               //发送给控制模块 [px4_pos_controller.cpp]的命令

Eigen::Vector3d mav_pos_;

void printf_result();
void groundtruth_cb(const nav_msgs::Odometry::ConstPtr& msg)
{
    GroundTruth = *msg;

    roi_point[0] = GroundTruth.pose.pose.position.x;
    roi_point[1] = GroundTruth.pose.pose.position.y;
    roi_point[2] = GroundTruth.pose.pose.position.z;
}
void landpad_det_cb(const prometheus_msgs::DetectionInfo::ConstPtr &msg)
{
    
    landpad_det.object_name = "landpad";
    landpad_det.Detection_info = *msg;
    // 此处相机的姿态一直在变化，因此返回的机体系位置不能直接使用
    // 根据云台姿态： 相机坐标系 to 集体坐标系

    // 识别算法发布的目标位置位于相机坐标系（从相机往前看，物体在相机右方x为正，下方y为正，前方z为正）
    // 相机安装误差 在mission_utils.h中设置
    landpad_det.pos_camera_frame[0] =  landpad_det.Detection_info.position[2] ;
    landpad_det.pos_camera_frame[1] = - landpad_det.Detection_info.position[0] ;
    landpad_det.pos_camera_frame[2] = - landpad_det.Detection_info.position[1] ;

    landpad_det.pos_body_frame = R_camera_to_body * landpad_det.pos_camera_frame;

    sight_angle[0] = landpad_det.Detection_info.sight_angle[0];
    sight_angle[1] = landpad_det.Detection_info.sight_angle[1];

    // 机体系 -> 机体惯性系 (原点在机体的惯性系) (对无人机姿态进行解耦)
    landpad_det.pos_body_enu_frame = R_Body_to_ENU * landpad_det.pos_body_frame;

    // 机体惯性系 -> 惯性系
    landpad_det.pos_enu_frame[0] = _DroneState.position[0] + landpad_det.pos_body_enu_frame[0];
    landpad_det.pos_enu_frame[1] = _DroneState.position[1] + landpad_det.pos_body_enu_frame[1];
    landpad_det.pos_enu_frame[2] = _DroneState.position[2] + landpad_det.pos_body_enu_frame[2];
    // 此降落方案不考虑偏航角 （高级版可提供）
    landpad_det.att_enu_frame[2] = 0.0;

    if(landpad_det.Detection_info.detected)
    {
        landpad_det.num_regain++;
        landpad_det.num_lost = 0;
    }else
    {
        landpad_det.num_regain = 0;
        landpad_det.num_lost++;
    }

    // 当连续一段时间无法检测到目标时，认定目标丢失
    if(landpad_det.num_lost > VISION_THRES)
    {
        landpad_det.is_detected = false;
    }

    // 当连续一段时间检测到目标时，认定目标得到
    if(landpad_det.num_regain > VISION_THRES)
    {
        landpad_det.is_detected = true;
    }

}
void drone_state_cb(const prometheus_msgs::DroneState::ConstPtr& msg)
{
    _DroneState = *msg;

    R_Body_to_ENU = get_rotation_matrix(_DroneState.attitude[0], _DroneState.attitude[1], _DroneState.attitude[2]);

    mav_pos_ << _DroneState.position[0],_DroneState.position[1],_DroneState.position[2];
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主函数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "gimbal_control");
    ros::NodeHandle nh("~");

    // 节点运行频率： 20hz 
    ros::Rate rate(20.0);

    
    bool moving_target;
    nh.param<bool>("hold_mode", hold_mode, false);
    nh.param<bool>("ignore_vision", ignore_vision, false);
    nh.param<bool>("moving_target", moving_target, false);
    //追踪控制参数
    nh.param<float>("kpx_track", kp_track[0], 0.1);
    nh.param<float>("kpy_track", kp_track[1], 0.1);
    nh.param<float>("kpz_track", kp_track[2], 0.1);
    nh.param<float>("kpyaw_track", kpyaw_track, 0.1);
    

    // 初始起飞点
    nh.param<float>("start_point_x", start_point[0], 0.0);
    nh.param<float>("start_point_y", start_point[1], 1.0);
    nh.param<float>("start_point_z", start_point[2], 1.5);

    gimbal_control gimbal_control_;
    

    //【订阅】地面真值，此信息仅做比较使用 不强制要求提供
    ros::Subscriber groundtruth_sub = nh.subscribe<nav_msgs::Odometry>("/ground_truth/landing_pad", 10, groundtruth_cb);

    //【订阅】降落板与无人机的相对位置及相对偏航角  单位：米   单位：弧度
    //  方向定义： 识别算法发布的目标位置位于相机坐标系（从相机往前看，物体在相机右方x为正，下方y为正，前方z为正）
    //  标志位：   detected 用作标志位 ture代表识别到目标 false代表丢失目标
    ros::Subscriber landpad_det_sub = nh.subscribe<prometheus_msgs::DetectionInfo>("/prometheus/object_detection/landpad_det", 10, landpad_det_cb);

    //【订阅】无人机状态
    ros::Subscriber drone_state_sub = nh.subscribe<prometheus_msgs::DroneState>("/prometheus/drone_state", 10, drone_state_cb);

    ros::Publisher message_pub = nh.advertise<prometheus_msgs::Message>("/prometheus/message/main", 10);

    //【发布】发送给控制模块 [px4_pos_controller.cpp]的命令
    ros::Publisher command_pub = nh.advertise<prometheus_msgs::ControlCommand>("/prometheus/control_command", 10);

    Command_Now.Command_ID = 1;
    Command_Now.source = NODE_NAME;

    float gimbal_att_sp_deg[3];


    bool sim_mode = true;
    if(sim_mode)
    {
        // Waiting for input
        int start_flag = 0;
        while(start_flag == 0)
        {
            cout << ">>>>>>>>>>>>>>>>>>>>>>>>>Gimbal Tracking Mission<<<<<<<<<<<<<<<<<<<<<< "<< endl;
            cout << "Please check the parameter and setting，enter 1 to continue， else for quit: "<<endl;
            cin >> start_flag;
        }

        while(ros::ok() && _DroneState.mode != "OFFBOARD")
        {
            Command_Now.header.stamp = ros::Time::now();
            Command_Now.Mode  = prometheus_msgs::ControlCommand::Idle;
            Command_Now.Command_ID = Command_Now.Command_ID + 1;
            Command_Now.source = NODE_NAME;
            Command_Now.Reference_State.yaw_ref = 999;
            command_pub.publish(Command_Now);   
            cout << "Switch to OFFBOARD and arm ..."<<endl;
            ros::Duration(2.0).sleep();
            ros::spinOnce();
        }
    }


    // 起飞
    pub_message(message_pub, prometheus_msgs::Message::WARN, NODE_NAME, "Takeoff to predefined position.");

    while( _DroneState.position[2] < 0.5)
    {      
        Command_Now.header.stamp                        = ros::Time::now();
        Command_Now.Mode                                = prometheus_msgs::ControlCommand::Move;
        Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
        Command_Now.source                              = NODE_NAME;
        Command_Now.Reference_State.Move_mode           = prometheus_msgs::PositionReference::XYZ_POS;
        Command_Now.Reference_State.Move_frame          = prometheus_msgs::PositionReference::ENU_FRAME;
        Command_Now.Reference_State.position_ref[0]     = start_point[0];
        Command_Now.Reference_State.position_ref[1]     = start_point[1];
        Command_Now.Reference_State.position_ref[2]     = start_point[2];
        Command_Now.Reference_State.yaw_ref             = 0.0;
        command_pub.publish(Command_Now);
        cout << "Takeoff to start point..."<<endl;
        ros::Duration(3.0).sleep();

        ros::spinOnce();
    }

    // 等待
    ros::Duration(3.0).sleep();

    while(ros::ok())
    {
        // 首先控制云台
        gimbal_att = gimbal_control_.get_gimbal_att();
        gimbal_att_deg = gimbal_att/PI*180;
        cout << "gimbal_att    : " << gimbal_att_deg[0] << " [deg] "<< gimbal_att_deg[1] << " [deg] "<< gimbal_att_deg[2] << " [deg] "<<endl;
        gimbal_att_rate = gimbal_control_.get_gimbal_att_rate();
        gimbal_att_rate_deg = gimbal_att_rate/PI*180;
        cout << "gimbal_att    : " << gimbal_att_rate_deg[0] << " [deg/s] "<< gimbal_att_rate_deg[1] << " [deg/s] "<< gimbal_att_rate_deg[2] << " [deg/s] "<<endl;
        R_camera_to_body = get_rotation_matrix(gimbal_att[0], gimbal_att[1], gimbal_att[2]);


        // 几个思考的点： 云台控制频率！！！ 设置定时函数，降低云台控制频率
        // 通过什么控制云台
        // 云台角度反馈如何控制无人机速度， 或者是加速度？ 是否需要积分项？
        // 多了姿态反馈后 如何使得与降落版垂直
        // 增加追踪人的场景
        if(ignore_vision)
        {
            Eigen::Vector3d error_vec;
            double distance_2d;
            error_vec = roi_point - mav_pos_;
            distance_2d = std::sqrt(error_vec(0) * error_vec(0) + error_vec(1) * error_vec(1));
            // 理想的吊舱控制情况
            gimbal_att_sp[0] = std::atan2(error_vec(2), distance_2d)/PI*180; //pitch
            gimbal_att_sp[1] = 0.0;
            gimbal_att_sp[2] = -std::atan2(error_vec(1), error_vec(0))/PI*180;//yaw
        }else
        {
            // 使用目标位置的视场角控制云台
            // 品灵吊舱原做法：根据像素来控制云台
            // 传统做法：使用视场角误差来反馈，控制吊舱角速度
            gimbal_att_sp[0] = gimbal_att_deg[0] - 0.1 * sight_angle[1]/PI*180; //pitch
            //gimbal_att_sp[0] = -sight_angle[1]/PI*180; //pitch
            // 云台滚转角不控制
            gimbal_att_sp[1] = 0.0;
            // 云台偏航角 取决于左右夹角
            gimbal_att_sp[2] = gimbal_att_deg[2] + 0.1 * sight_angle[0]/PI*180; //pitch
            //gimbal_att_sp[2] = sight_angle[0]/PI*180;
        }

        gimbal_control_.send_mount_control_command(gimbal_att_sp);

        
        // 无人机追踪目标
        if (!hold_mode)
        {
            if(landpad_det.is_detected)
            {
                Command_Now.header.stamp                        = ros::Time::now();
                Command_Now.Mode                                = prometheus_msgs::ControlCommand::Move;
                Command_Now.Command_ID                          = Command_Now.Command_ID + 1;
                Command_Now.source                              = NODE_NAME;
                Command_Now.Reference_State.Move_mode           = prometheus_msgs::PositionReference::XYZ_VEL;
                Command_Now.Reference_State.Move_frame          = prometheus_msgs::PositionReference::ENU_FRAME;
                Command_Now.Reference_State.velocity_ref[0]     = kp_track[0] * (GroundTruth.pose.pose.position.x - 1.0 - mav_pos_[0]); // 暂时使用真值
                if(moving_target)
                {
                    Command_Now.Reference_State.velocity_ref[0] += 0.3;
                }
                // 如果不控制无人机偏航角（即yaw_ref = 0），可根据云台偏航角控制无人机y轴速度
                // Command_Now.Reference_State.velocity_ref[1]     = 0.0;
                // y轴速度应当根据视觉解算的目标姿态来调整？ 待定
                Command_Now.Reference_State.velocity_ref[1]     = - kp_track[1] * gimbal_att_sp[2];
                // z轴速度取决与当前云台俯仰角度（俯仰角速度） 注意gimbal_att_sp的角度是deg
                // 高度上保持一致，还是保持一个10度俯仰角
                Command_Now.Reference_State.velocity_ref[2]     = kp_track[2] * (gimbal_att_sp[0] + 2);   
                // 偏航角 取决于当前云台偏航角
                Command_Now.Reference_State.Yaw_Rate_Mode       = false;
                //Command_Now.Reference_State.yaw_ref        = - gimbal_att_sp[2]/180*PI;
                Command_Now.Reference_State.yaw_ref        = 0.0;
                Command_Now.Reference_State.yaw_rate_ref        = - kpyaw_track * gimbal_att_sp[2];
            }else
            {
                Command_Now.header.stamp = ros::Time::now();
                Command_Now.Command_ID   = Command_Now.Command_ID + 1;
                Command_Now.source = NODE_NAME;
                Command_Now.Mode = prometheus_msgs::ControlCommand::Hold;
            }


            command_pub.publish(Command_Now);
        }


        printf_result();
        ros::spinOnce();
        rate.sleep();
    }

    return 0;

}

void printf_result()
{
    //固定的浮点显示
    cout.setf(ios::fixed);
    //setprecision(n) 设显示小数精度为n位
    cout<<setprecision(2);
    //左对齐
    cout.setf(ios::left);
    // 强制显示小数点
    cout.setf(ios::showpoint);
    // 强制显示符号
    cout.setf(ios::showpos);

    cout << ">>>>>>>>>>>>>>>>>>>>>>Autonomous Landing Mission<<<<<<<<<<<<<<<<<<<"<< endl;

    cout << ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>Vision State<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;
    if(landpad_det.is_detected)
    {
        cout << "is_detected: ture" <<endl;
    }else
    {
        cout << "is_detected: false" <<endl;
    }
    
    cout << "Target_pos (camera): " << landpad_det.pos_camera_frame[0] << " [m] "<< landpad_det.pos_camera_frame[1] << " [m] "<< landpad_det.pos_camera_frame[2] << " [m] "<<endl;
    cout << "Target_pos (body): " << landpad_det.pos_body_frame[0] << " [m] "<< landpad_det.pos_body_frame[1] << " [m] "<< landpad_det.pos_body_frame[2] << " [m] "<<endl;



    cout << "Target_pos (body_enu): " << landpad_det.pos_body_enu_frame[0] << " [m] "<< landpad_det.pos_body_enu_frame[1] << " [m] "<< landpad_det.pos_body_enu_frame[2] << " [m] "<<endl;

    cout << "Ground_truth(pos):  " << GroundTruth.pose.pose.position.x << " [m] "<< GroundTruth.pose.pose.position.y << " [m] "<< GroundTruth.pose.pose.position.z << " [m] "<<endl;
    cout << "Detection_ENU(pos): " << landpad_det.pos_enu_frame[0] << " [m] "<< landpad_det.pos_enu_frame[1] << " [m] "<< landpad_det.pos_enu_frame[2] << " [m] "<<endl;
    cout << "Detection_ENU(yaw): " << landpad_det.att_enu_frame[2]/3.1415926 *180 << " [deg] "<<endl;

}

