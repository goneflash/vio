#include "map.hpp"

#include <iostream>

namespace vio {

Map::Map()
  : map_state_(WAIT_FOR_FIRSTFRAME) {}

bool Map::AddFirstKeyframe(std::unique_ptr<Keyframe> frame) {
  if (map_state_ != WAIT_FOR_FIRSTFRAME) {
    std::cerr << "Error: First Keyframe already exists. Try reset map.\n";
    return false;
  }
  keyframe_id_to_index_[frame->frame_id()] = 0;
  keyframes_.push_back(std::move(frame));
  // Set pose of first frame
  cv::Mat R = cv::Mat::eye(3, 3, CV_64F);
  cv::Mat t = cv::Mat::ones(3, 1, CV_64F);
  keyframes_[0]->set_pose(R, t);

  map_state_ = WAIT_FOR_SECONDFRAME;
  uninited_landmark_range_start_ = 0;
}

bool Map::AddNewKeyframeMatchToLastKeyframe(std::unique_ptr<Keyframe> frame,
                                            std::vector<cv::DMatch> &matches) {
  if (map_state_ == WAIT_FOR_FIRSTFRAME) {
    std::cerr << "Error: Missing first frame.\n";
    return false;
  }
  keyframe_id_to_index_[frame->frame_id()] = keyframes_.size();
  keyframes_.push_back(std::move(frame));

  // -------------- Add new match edge
  FeatureMatchEdge match_edge;
  match_edge.first_frame_index = keyframes_.size() - 2;
  match_edge.second_frame_index = keyframes_.size() - 1;

  const int l_index = match_edge.first_frame_index;   // last frame index
  const int n_index = match_edge.second_frame_index;  // new frame index
  feature_to_landmark_.resize(n_index + 1);

  for (int i = 0; i < matches.size(); ++i) {
    // Find existing landmark
    auto ld_id_ptr = feature_to_landmark_[l_index].find(matches[i].queryIdx);
    // If the feature is not a landmark yet
    if (ld_id_ptr == feature_to_landmark_[l_index].end()) {
      // Add a new landmark
      const int landmark_id = landmark_to_feature_.size();
      landmark_to_feature_.resize(landmark_id + 1);
      landmark_to_feature_.back()[n_index] = matches[i].trainIdx;
      landmark_to_feature_.back()[l_index] = matches[i].queryIdx;
      feature_to_landmark_[l_index][matches[i].queryIdx] = landmark_id;
      feature_to_landmark_[n_index][matches[i].trainIdx] = landmark_id;

      // New uninitialized points added.
      uninited_landmark_range_end_ = landmark_to_feature_.size() - 1;
    } else {
      int landmark_id = ld_id_ptr->second;
      // Link a feature to a landmark
      feature_to_landmark_[n_index][matches[i].trainIdx] = landmark_id;
      // Link a landmark to a feature in the new frame
      landmark_to_feature_[landmark_id][n_index] = matches[i].trainIdx;
    }
  }

  match_edge.matches = std::move(matches);
  if (map_state_ == WAIT_FOR_SECONDFRAME) map_state_ = WAIT_FOR_INIT;

  return true;
}

bool Map::PrepareInitializationData(
    std::vector<std::vector<cv::Vec2d> > &feature_vectors) {
  if (map_state_ != WAIT_FOR_INIT) {
    std::cerr << "Error: Could not initialize.\n";
    return false;
  }
  std::cout << "Prepare initialization data for " << keyframes_.size()
            << " frames.\n";
  feature_vectors.resize(keyframes_.size());

  const int start_frame_id = 0;
  const int end_frame_id = keyframes_.size() - 1;
  const int num_landmark = landmark_to_feature_.size();

  // Initialize feature vector
  // TODO: Optimize, if the initialization cost much time.
  for (int frame_id = 0; frame_id < keyframes_.size(); ++frame_id) {
    feature_vectors[frame_id].resize(num_landmark, cv::Vec2d(-1, -1));
  }
  for (int ld_id = 0; ld_id < num_landmark; ++ld_id) {
    for (auto &ld_feature_id : landmark_to_feature_[ld_id]) {
      const cv::KeyPoint &kp = (keyframes_[ld_feature_id.first]
                                    ->image_frame()
                                    .keypoints())[ld_feature_id.second];
      feature_vectors[ld_feature_id.first][ld_id] = cv::Vec2d(kp.pt.x, kp.pt.y);
    }
  }
  return true;
}

bool Map::AddInitialization(const std::vector<cv::Point3f> &points3d,
                            const std::vector<bool> &points3d_mask,
                            const std::vector<cv::Mat> &Rs,
                            const std::vector<cv::Mat> &ts) {
  if (map_state_ != WAIT_FOR_INIT) {
    std::cerr << "Error: Could not add initialization.\n";
    return false;
  }
  if (Rs.size() != keyframes_.size() || ts.size() != keyframes_.size() ||
      points3d.size() != landmark_to_feature_.size() ||
      points3d_mask.size() != landmark_to_feature_.size()) {
    std::cerr << "Error: Initialization data doesn't match Map data.\n";
    return false;
  }

  // TODO: Refactor. Only add keyframe after initialization.

  // TODO: Use pprof to see where is the bottleneck
  // TODO: Try use GPU to replace the bottleneck
  std::vector<std::unordered_map<int, int> > inited_landmark_to_feature;
  for (int ld_id = 0; ld_id < points3d.size(); ++ld_id) {
    if (points3d_mask[ld_id]) {
      inited_landmark_to_feature.push_back(
          std::move(landmark_to_feature_[ld_id]));

      Landmark new_ld;
      new_ld.position = points3d[ld_id];

      landmarks_.push_back(new_ld);
    } else {
      for (auto &feature_in_frame : landmark_to_feature_[ld_id]) {
        const int frame_id = feature_in_frame.first;
        const int feature_id = feature_in_frame.second;
        feature_to_landmark_[frame_id].erase(
            feature_to_landmark_[frame_id].find(feature_id));
      }
    }
  }
  std::cout << "Cleaned up "
            << landmark_to_feature_.size() - inited_landmark_to_feature.size()
            << " landmarks.";
  landmark_to_feature_ = std::move(inited_landmark_to_feature);

  uninited_landmark_range_start_ = landmark_to_feature_.size();
  uninited_landmark_range_end_ = landmark_to_feature_.size();

  for (int i = 0; i < keyframes_.size(); ++i) {
    keyframes_[i]->set_pose(Rs[i], ts[i]);
    keyframes_[i]->set_pose_inited(true);
  }

  map_state_ = INITIALIZED;
  return true;
}

bool Map::PrepareEstimateLastFramePoseData(std::vector<cv::Point3f> &points3d,
                                           std::vector<cv::Point2f> &points2d,
                                           std::vector<int> &points_index) {
  if (map_state_ != INITIALIZED) {
    std::cerr << "Error: Map not initialized yet.\n";
    return false;
  }
  if (keyframes_.back()->pose_inited()) {
    std::cerr << "Error: Last frame already inited pose.\n";
    return false;
  }

  points3d.clear();
  points2d.clear();
  points_index.clear();

  for (auto &feature : feature_to_landmark_.back()) {
    const int feature_id = feature.first;
    const int ld_id = feature.second;
    if (ld_id >= uninited_landmark_range_start_) continue;
    points_index.push_back(feature_id);
    points3d.push_back(landmarks_[ld_id].position);
    points2d.push_back(
        keyframes_.back()->image_frame().keypoints()[feature_id].pt);
  }

  std::cout << "Found " << points_index.size() << " 2d to 3d match for pnp.\n";
  keyframes_.back()->set_pose_inited(true);

  return true;
}

bool Map::SetLastFramePose(const cv::Mat &R, const cv::Mat &t) {
  keyframes_.back()->set_pose(R, t);
};

}  // vio
