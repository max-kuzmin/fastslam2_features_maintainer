#include <ros/ros.h>
#include <sensor_msgs/LaserScan.h>
#include <sensor_msgs/PointCloud2.h>
#include <laser_geometry/laser_geometry.h>
#include <pointmatcher_ros/point_cloud.h>
#include <pointmatcher/PointMatcher.h>
#include <pointmatcher_ros/transform.h>
#include <tf/transform_listener.h>


using namespace PointMatcherSupport;
using namespace std;

typedef PointMatcher<float> PM;
typedef PM::DataPoints DP;

//Publishers
ros::Publisher scanCloudPublisher;
ros::Publisher inliersCloudPublisher;
ros::Publisher mapCloudPublisher;
ros::Publisher partialMapCloudPublisher;

//Listener for pose to map transformation
tf::TransformListener* tfListener;

//Params
double cutOffRange = 3.9;
double rateHZ = 5;
string mapFrame = "map";
string poseFrame = "base_scan";
double initPointProbability = 0.8; //0.99
double delPointThreshold = 0;
double inlierPointProbability = 0.99; //0.8
double outlierPointProbability = 0.1; //0.1
bool saveToVTK = false;
int knnRead = 20; //kd-tree //10
int knnRef = 1; //1
double maxScanDensity = 100000; //points per m3
int minPointsToMatch = max(knnRef, knnRead);

DP mapPoints;

//Functions
void scan_callback(const sensor_msgs::LaserScan& msg);
pair<DP,DP> filter_transform_map_scan(DP& readPoints); //return: transformed scan and partial map
DP match_clouds(DP& read, DP& ref); //return: scan outliers
void update_map(DP& scanOutliers, DP& partialMap);

double calc_point_weight(double probability, double prev=0) {
    return prev + log(probability / (1.0 - probability));
}

// ===== Create filters and transformations =====

PM::DataPointsFilter* normalsFlter(PM::get().DataPointsFilterRegistrar.create(
    "SurfaceNormalDataPointsFilter",
    map_list_of("keepNormals", toParam(0))
               ("keepDensities", toParam(1))
));

PM::Transformation* rigidTrans = PM::get().REG(Transformation).create("RigidTransformation");

PM::DataPointsFilter* densityFlter(PM::get().DataPointsFilterRegistrar.create(
    "MaxDensityDataPointsFilter",
    map_list_of("maxDensity", toParam(maxScanDensity))
));

PM::DataPointsFilter* maxDistFlter(PM::get().DataPointsFilterRegistrar.create(
    "MaxDistDataPointsFilter",
    map_list_of("maxDist", toParam(cutOffRange))
));

PM::DataPointsFilter* minDistFlter(PM::get().DataPointsFilterRegistrar.create(
    "MinDistDataPointsFilter",
    map_list_of("minDist", toParam(cutOffRange))
));

PM::Matcher *matcherRead(PM::get().MatcherRegistrar.create(
    "KDTreeMatcher",
    map_list_of("knn", toParam(knnRead))));

PM::Matcher *matcherReadToTarget(PM::get().MatcherRegistrar.create(
    "KDTreeVarDistMatcher",
    map_list_of("knn", toParam(knnRef))
               ("maxDistField","maxSearchDist") // descriptor name
    ));

// ==========

int main(int argc, char** argv)
{
    ros::init(argc, argv, "features_maintainer_node");

    //Create node and topics
    ros::NodeHandle node("~");
    tfListener = new tf::TransformListener();
    scanCloudPublisher = node.advertise<sensor_msgs::PointCloud2>("/scan_cloud", 10);
    inliersCloudPublisher = node.advertise<sensor_msgs::PointCloud2>("/matched_cloud", 10);
    mapCloudPublisher = node.advertise<sensor_msgs::PointCloud2>("/map_cloud", 10);
    partialMapCloudPublisher = node.advertise<sensor_msgs::PointCloud2>("/partial_map_cloud", 10);
    ros::Subscriber scanSubscriber = node.subscribe("/scan", 1, scan_callback);

    ros::Rate rate(rateHZ);
    while (ros::ok()) {        
        ros::spinOnce();
        rate.sleep();
    }

    return 0;
};





void scan_callback(const sensor_msgs::LaserScan& msg)
{

     //========== Convert scan points cloud ==========

    //Convert scan to ROS points cloud
    sensor_msgs::PointCloud2 cloud;
    laser_geometry::LaserProjection projector;
    projector.projectLaser(msg, cloud, cutOffRange);

    //Convert ROS points cloud to libpointmatcher cloud
    DP readPoints = PointMatcher_ros::rosMsgToPointMatcherCloud<float>(cloud);
    //if no poits in scan - return
    if (readPoints.getNbPoints()<minPointsToMatch) return;


    //========== Transform scan ==========

    //filter density of scan, transform to pose
    //select map points with some radius, remove them from mapPoints
    pair<DP,DP> clouds = filter_transform_map_scan(readPoints);
    DP readTransformedPoints = clouds.first, partialMap = clouds.second;


    //========== Create map ==========

    //Create map if empty
    if (mapPoints.getNbPoints() == 0) {
        mapPoints = DP(readTransformedPoints);
        PM::Matrix weightsMap = PM::Matrix::Constant(1, mapPoints.features.cols(), calc_point_weight(initPointProbability));
        mapPoints.addDescriptor("weights", weightsMap);
    }

    //========== Update map ==========

    //If not enough points in partial map to match add all points from scan to full map
    else if (partialMap.getNbPoints() < minPointsToMatch){
        update_map(readTransformedPoints, partialMap);
    }
    else {
        //Find matches of transformed scan and selected points of map
        DP readOutliers = match_clouds(readTransformedPoints, partialMap);

        //Update weights of map points, append scan outliers
        update_map(readOutliers, partialMap);
    }
}





pair<DP,DP> filter_transform_map_scan(DP& readPoints) {

    //========== Get and check pose transformation params ==========

    PM::TransformationParameters robotPoseTrans;
    try {
        //Get transformation of new points cloud to map
        robotPoseTrans = PointMatcher_ros::transformListenerToEigenMatrix<float>(*tfListener, mapFrame, poseFrame, ros::Time(0));
    }
    catch (tf2::TransformException& ex)
    {
        ROS_WARN("%s", ex.what());
        return make_pair(DP(), DP());
    }

    if (!rigidTrans->checkParameters(robotPoseTrans)) {
        std::cout << "WARNING: T does not represent a valid rigid transformation"<< std::endl;
        return make_pair(DP(), DP());
    }

    //========== Filter and transform scan ==========

    //Calc densities descriptor
    readPoints = normalsFlter->filter(readPoints);

    //Filter scan by density
    readPoints = densityFlter->filter(readPoints);

    readPoints.removeDescriptor("densities");

    // Compute the transformation from pose to map
    DP readTransformedPoints =  rigidTrans->compute(readPoints, robotPoseTrans);
    //Publish to ROS
    scanCloudPublisher.publish(
        PointMatcher_ros::pointMatcherCloudToRosMsg<float>(readTransformedPoints, mapFrame, ros::Time(0)));


    //========== Filter and transform map ==========

    //Transform map to origin
    PM::TransformationParameters robotPoseTransInv = robotPoseTrans.inverse();
    DP mapAtOrigin = rigidTrans->compute(mapPoints, robotPoseTransInv);

    //Filter map by radius - get only closest points
    DP partialMap = maxDistFlter->filter(mapAtOrigin);

    if (partialMap.getNbPoints() == 0)
        return make_pair(readTransformedPoints, DP());

    partialMap = rigidTrans->compute(partialMap, robotPoseTrans);

    //Filter map by radius - get least of map
    mapPoints = minDistFlter->filter(mapAtOrigin);
    mapPoints = rigidTrans->compute(mapPoints, robotPoseTrans);

    //Publish to ROS partial map
    partialMapCloudPublisher.publish(
        PointMatcher_ros::pointMatcherCloudToRosMsg<float>(partialMap, mapFrame, ros::Time(0)));

    return make_pair(readTransformedPoints, partialMap);
}





DP match_clouds(DP& read, DP& ref) {

    // ========== Init ==========

    int readPtsCount = read.getNbPoints();
    int refPtsCount = ref.getNbPoints();

    // Create the default ICP algorithm
    PM::ICP icp;
    icp.setDefault();

    // ========== Match points with ICP ==========

    // Compute the transformation to express readPoints in mapPoints
    PM::TransformationParameters T = icp(read, ref);

    // Transform readPoints to express it in mapPoints
    DP readPointsTransformed(read);
    icp.transformations.apply(readPointsTransformed, T);

    // ========== Find matches ==========


    // Matcher to find closest points on read scan
    matcherRead->init(read);

    // Find clothest points on read
    PM::Matches readMatches(knnRead, readPtsCount);
    readMatches = matcherRead->findClosests(read);

    // For every point set maximum search distanse as square root of distanse to
    // farest point, found by matcherRead
    PM::Matrix maxSearchDist = readMatches.dists.colwise().maxCoeff().cwiseSqrt();
    read.addDescriptor("maxSearchDist", maxSearchDist);

    // Matcher to match points on both scans with limit to max distance
    matcherReadToTarget->init(ref);

    // Find matches from read to ref
    PM::Matches refMatches(knnRef, refPtsCount);
        refMatches = matcherReadToTarget->findClosests(read);

    // Add new descriptors to select inliers and outliers
    PM::Matrix inliersRead = PM::Matrix::Zero(1, read.getNbPoints());
    read.addDescriptor("inliers", inliersRead);
    PM::Matrix inliersRef = PM::Matrix::Zero(1, ref.getNbPoints());
    ref.addDescriptor("inliers", inliersRef);

    // Get view to edit inliers
    DP::View inlierRead = read.getDescriptorViewByName("inliers");
    DP::View inlierRef = ref.getDescriptorViewByName("inliers");

    // For every point
    for (int i = 0; i < readPtsCount; i++) {
      // For every match
      for (int k = 0; k < knnRef; k++) {

        if (refMatches.dists(k, i) != PM::Matches::InvalidDist) {
          // Set inlier descriptor to both points
          inlierRead(0, i) = 1.0;
          inlierRef(0, refMatches.ids(k, i)) = 1.0;
        }

      }
    }

    // ========== Filter outliers ==========

    // Copy read outliers and inliers to new clouds
    DP readOutliers = read.createSimilarEmpty();
    int readOutliersCount = 0;
    int readInliersCount = 0;
    for (int i = 0; i < readPtsCount; i++) {
        if (inlierRead(0,i)==0) {
            readOutliers.features.col(readOutliersCount) = read.features.col(i);
            readOutliers.descriptors.col(readOutliersCount) = read.descriptors.col(i);
            readOutliersCount++;
        }
        else {
            readInliersCount++;
        }
    }
    readOutliers.conservativeResize(readOutliersCount);

    // Copy map outliers and inliers to new clouds
    DP refInliers = ref.createSimilarEmpty();
    int refOutliersCount = 0;
    int refInliersCount = 0;
    for (int i = 0; i < refPtsCount; i++) {
        if (inlierRef(0,i)==0) {
            refOutliersCount++;
        }
        else {
            refInliers.features.col(refInliersCount) = ref.features.col(i);
            refInliers.descriptors.col(refInliersCount) = ref.descriptors.col(i);
            refInliersCount++;
        }
    }
    refInliers.conservativeResize(refInliersCount);

    // ========== Update map ==========

    sensor_msgs::PointCloud2 inliersCloud = PointMatcher_ros::pointMatcherCloudToRosMsg<float>(refInliers, mapFrame, ros::Time(0));
    inliersCloudPublisher.publish(inliersCloud);

    // ========== Debug ==========

    stringstream ss;
    ss << endl <<
          "read: all=" << read.getNbPoints() <<
          ", in=" << readInliersCount <<
          ", out=" << readOutliersCount << endl <<
          "ref:  all=" << ref.getNbPoints() <<
          ", in=" << refInliersCount <<
          ", out=" << refOutliersCount << endl <<
          "map:  all=" << mapPoints.getNbPoints();
    ROS_INFO("%s", ss.str().c_str());

    if (saveToVTK) {
        read.save("read_orig.vtk");
        mapPoints.save("map.vtk");
        ref.save("ref.vtk");
        refInliers.save("ref_inliers.vtk");
        readOutliers.save("read_outliers.vtk");
    }

    return readOutliers;

}





void update_map(DP& scanOutliers, DP& partialMap) {

    int mapSize = partialMap.getNbPoints();

    // ========== Update weights of map  ==========

    //if points from map were not matched (because them not enough), do not update their weights
    if (mapSize>=minPointsToMatch) {

        DP::View mapWeightsDesc = partialMap.getDescriptorViewByName("weights");
        DP::View mapInliersDesc = partialMap.getDescriptorViewByName("inliers");

        // For map inliers add weight, for outliers sub weight
        for (int i=0; i<mapSize; i++) {
            if (mapInliersDesc(0,i) == 1.0)
                mapWeightsDesc(0,i) += calc_point_weight(inlierPointProbability);
            else
                mapWeightsDesc(0,i) += calc_point_weight(outlierPointProbability);
        }

        //Remove points from map that weight less than threshold
        DP newMapPoints = partialMap.createSimilarEmpty();
        int count = 0;
        for (int i = 0; i < mapSize; i++) {
            if (mapWeightsDesc(0,i)>delPointThreshold) {
                newMapPoints.features.col(count) = partialMap.features.col(i);
                newMapPoints.descriptors.col(count) = partialMap.descriptors.col(i);
                count++;
            }
        }
        newMapPoints.conservativeResize(count);

        //Add partial map to least of map
        newMapPoints.removeDescriptor("inliers");
        mapPoints.concatenate(newMapPoints);
    }

    // ========== Add scan outliers to map  ==========

    PM::Matrix weightsScanOutliers = PM::Matrix::Constant(1, scanOutliers.getNbPoints(), calc_point_weight(initPointProbability));
    scanOutliers.addDescriptor("weights", weightsScanOutliers);
    scanOutliers.removeDescriptor("inliers");
    mapPoints.concatenate(scanOutliers);

    // ========== Publish to ROS ==========

    sensor_msgs::PointCloud2 mapCloud = PointMatcher_ros::pointMatcherCloudToRosMsg<float>(mapPoints, mapFrame, ros::Time(0));
    mapCloudPublisher.publish(mapCloud);


}
