//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2017 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

//----------------------------------------------------------------------------//
// Initial gltf2ozz implementation author: Alexander Dzhoganov                //
// https://github.com/guillaumeblanc/ozz-animation/pull/70                    //
//----------------------------------------------------------------------------//

#include "ozz/animation/offline/tools/import2ozz.h"
#include "ozz/animation/runtime/skeleton.h"

#include "ozz/base/containers/map.h"
#include "ozz/base/containers/set.h"

#include "ozz/base/log.h"

#include <cassert>
#include <cstring>

#define TINYGLTF_IMPLEMENTATION

// No support for image loading or writing
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)  // unreachable code
#pragma warning(disable : 4267)  // conversion from 'size_t' to 'type'
#endif                           // _MSC_VER

#include "extern/tiny_gltf.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif  // _MSC_VER

namespace {

template <typename _VectorType>
bool FixupNames(_VectorType& _data, const char* _pretty_name,
                const char* _prefix_name) {
  ozz::Set<std::string>::Std names;
  for (size_t i = 0; i < _data.size(); ++i) {
    bool renamed = false;
    typename _VectorType::const_reference data = _data[i];

    std::string name(data.name.c_str());

    // Fixes unnamed animations.
    if (name.length() == 0) {
      renamed = true;
      name = _prefix_name;
      name += std::to_string(i);
    }

    // Fixes duplicated names, while it has duplicates
    for (auto it = names.find(name); it != names.end(); it = names.find(name)) {
      renamed = true;
      name += "_";
      name += std::to_string(i);
    }

    // Update names index.
    if (!names.insert(name).second) {
      assert(false && "Algorithm must ensure no duplicated animation names.");
    }

    if (renamed) {
      ozz::log::LogV() << _pretty_name << " #" << i << " with name \""
                       << data.name << "\" was renamed to \"" << name
                       << "\" in order to avoid duplicates." << std::endl;

      // Actually renames tinygltf data.
      _data[i].name = name;
    }
  }

  return true;
}

// Returns the address of a gltf buffer given an accessor.
// Performs basic checks to ensure the data is in the correct format
template <typename T>
const T* BufferView(const tinygltf::Model& _model,
                    const tinygltf::Accessor& _accessor) {
  const int32_t component_size =
      tinygltf::GetComponentSizeInBytes(_accessor.componentType);
  const int32_t element_size =
      component_size * tinygltf::GetTypeSizeInBytes(_accessor.type);
  if (element_size != sizeof(T)) {
    ozz::log::Err() << "Invalid buffer view access. Expected element size '"
                    << sizeof(T) << " got " << element_size << " instead."
                    << std::endl;
    return nullptr;
  }

  const tinygltf::BufferView& bufferView = _model.bufferViews[_accessor.bufferView];
  const tinygltf::Buffer& buffer = _model.buffers[bufferView.buffer];
  return reinterpret_cast<const T*>(buffer.data.data() + bufferView.byteOffset +
                                    _accessor.byteOffset);
}

// Samples a linear animation channel
// There is an exact mapping between gltf and ozz keyframes so we just copy
// everything over.
template <typename _KeyframesType>
bool SampleLinearChannel(const tinygltf::Model& _model,
                         const tinygltf::Accessor& _output,
                         const float* _timestamps, _KeyframesType* _keyframes) {
  typedef typename _KeyframesType::value_type::Value ValueType;

  const ValueType* values = BufferView<ValueType>(_model, _output);
  if (values == nullptr) {
    return false;
  }

  _keyframes->resize(_output.count);
  for (size_t i = 0; i < _output.count; ++i) {
    typename _KeyframesType::reference key = _keyframes->at(i);
    key.time = _timestamps[i];
    key.value = values[i];
  }

  return true;
}

// Samples a step animation channel
// There are twice as many ozz keyframes as gltf keyframes
template <typename _KeyframesType>
bool SampleStepChannel(const tinygltf::Model& _model,
                       const tinygltf::Accessor& _output,
                       const float* timestamps, _KeyframesType* _keyframes) {
  typedef typename _KeyframesType::value_type::Value ValueType;
  const ValueType* values = BufferView<ValueType>(_model, _output);
  if (values == nullptr) {
    return false;
  }

  // A step is created with 2 consecutive keys. Last step is a single key.
  size_t numKeyframes = _output.count * 2 - 1;
  _keyframes->resize(numKeyframes);

  const float eps = 1e-6f;

  for (size_t i = 0; i < _output.count; i++) {
    typename _KeyframesType::reference key = _keyframes->at(i * 2);
    key.time = timestamps[i];
    key.value = values[i];

    if (i < _output.count - 1) {
      typename _KeyframesType::reference next_key = _keyframes->at(i * 2 + 1);
      next_key.time = timestamps[i + 1] - eps;
      next_key.value = values[i];
    }
  }

  return true;
}

// Samples a hermite spline in the form
// p(t) = (2t^3 - 3t^2 + 1)p0 + (t^3 - 2t^2 + t)m0 + (-2t^3 + 3t^2)p1 + (t^3 -
// t^2)m1 where t is a value between 0 and 1 p0 is the starting point at t = 0
// m0 is the scaled starting tangent at t = 0
// p1 is the ending point at t = 1
// m1 is the scaled ending tangent at t = 1
// p(t) is the resulting point value
template <typename T>
T SampleHermiteSpline(float t, const T& p0, const T& m0, const T& p1,
                      const T& m1) {
  const float t2 = t * t;
  const float t3 = t2 * t;

  // a = 2t^3 - 3t^2 + 1
  const float a = 2.0f * t3 - 3.0f * t2 + 1.0f;
  // b = t^3 - 2t^2 + t
  const float b = t3 - 2.0f * t2 + t;
  // c = -2t^3 + 3t^2
  const float c = -2.0f * t3 + 3.0f * t2;
  // d = t^3 - t^2
  const float d = t3 - t2;

  // p(t) = a * p0 + b * m0 + c * p1 + d * m1
  T pt = p0 * a + m0 * b + p1 * c + m1 * d;
  return pt;
}

// Samples a cubic-spline channel
// the number of keyframes is determined from the animation duration and given
// sample rate
template <typename _KeyframesType>
bool SampleCubicSplineChannel(const tinygltf::Model& _model,
                              const tinygltf::Accessor& _output,
                              const float* _timestamps,
                              _KeyframesType* _keyframes, float _sampling_rate,
                              float duration) {
  typedef typename _KeyframesType::value_type::Value ValueType;
  const ValueType* values = BufferView<ValueType>(_model, _output);
  if (values == nullptr) {
    return false;
  }

  assert(_output.count % 3 == 0);
  size_t numKeyframes = _output.count / 3;

  // TODO check size matches
  _keyframes->resize(
      static_cast<size_t>(floor(duration * _sampling_rate) + 1.f));
  size_t currentKey = 0;

  for (size_t i = 0; i < _keyframes->size(); i++) {
    float time = (float)i / _sampling_rate;
    while (_timestamps[currentKey] > time && currentKey < numKeyframes - 1) {
      currentKey++;
    }

    float currentTime = _timestamps[currentKey];   // current keyframe time
    float nextTime = _timestamps[currentKey + 1];  // next keyframe time

    float t = (time - currentTime) / (nextTime - currentTime);
    const ValueType& p0 = values[currentKey * 3 + 1];
    const ValueType m0 = values[currentKey * 3 + 2] * (nextTime - currentTime);
    const ValueType& p1 = values[(currentKey + 1) * 3 + 1];
    const ValueType m1 =
        values[(currentKey + 1) * 3] * (nextTime - currentTime);

    typename _KeyframesType::reference key = _keyframes->at(i);
    key.time = time;
    key.value = SampleHermiteSpline(t, p0, m0, p1, m1);
  }

  return true;
}

ozz::animation::offline::RawAnimation::TranslationKey
CreateTranslationBindPoseKey(const tinygltf::Node& _node) {
  ozz::animation::offline::RawAnimation::TranslationKey key;
  key.time = 0.0f;

  if (_node.translation.empty()) {
    key.value = ozz::math::Float3::zero();
  } else {
    key.value = ozz::math::Float3(static_cast<float>(_node.translation[0]),
                                  static_cast<float>(_node.translation[1]),
                                  static_cast<float>(_node.translation[2]));
  }

  return key;
}

ozz::animation::offline::RawAnimation::RotationKey CreateRotationBindPoseKey(
    const tinygltf::Node& _node) {
  ozz::animation::offline::RawAnimation::RotationKey key;
  key.time = 0.0f;

  if (_node.rotation.empty()) {
    key.value = ozz::math::Quaternion::identity();
  } else {
    key.value = ozz::math::Quaternion(static_cast<float>(_node.rotation[0]),
                                      static_cast<float>(_node.rotation[1]),
                                      static_cast<float>(_node.rotation[2]),
                                      static_cast<float>(_node.rotation[3]));
  }
  return key;
}

ozz::animation::offline::RawAnimation::ScaleKey CreateScaleBindPoseKey(
    const tinygltf::Node& _node) {
  ozz::animation::offline::RawAnimation::ScaleKey key;
  key.time = 0.0f;

  if (_node.scale.empty()) {
    key.value = ozz::math::Float3::one();
  } else {
    key.value = ozz::math::Float3(static_cast<float>(_node.scale[0]),
                                  static_cast<float>(_node.scale[1]),
                                  static_cast<float>(_node.scale[2]));
  }
  return key;
}

// Creates the default transform for a gltf node
bool CreateNodeTransform(const tinygltf::Node& _node,
                         ozz::math::Transform* _transform) {
  if (_node.matrix.size() != 0) {
    // For animated nodes matrix should never be set
    // From the spec: "When a node is targeted for animation (referenced by an
    // animation.channel.target), only TRS properties may be present; matrix
    // will not be present."
    ozz::log::Err() << "Node \"" << _node.name
                    << "\" transformation matrix is not empty. This is "
                       "disallowed by the glTF spec as this node is an "
                       "animation target."
                    << std::endl;
    return false;
  }

  *_transform = ozz::math::Transform::identity();

  if (!_node.translation.empty()) {
    _transform->translation =
        ozz::math::Float3(static_cast<float>(_node.translation[0]),
                          static_cast<float>(_node.translation[1]),
                          static_cast<float>(_node.translation[2]));
  }
  if (!_node.rotation.empty()) {
    _transform->rotation =
        ozz::math::Quaternion(static_cast<float>(_node.rotation[0]),
                              static_cast<float>(_node.rotation[1]),
                              static_cast<float>(_node.rotation[2]),
                              static_cast<float>(_node.rotation[3]));
  }
  if (!_node.scale.empty()) {
    _transform->scale = ozz::math::Float3(static_cast<float>(_node.scale[0]),
                                          static_cast<float>(_node.scale[1]),
                                          static_cast<float>(_node.scale[2]));
  }

  return true;
}
}  // namespace

class GltfImporter : public ozz::animation::offline::OzzImporter {
 public:
  GltfImporter() {
    // We don't care about image data but we have to provide this callback
    // because we're not loading the stb library
    auto image_loader = [](tinygltf::Image*, const int, std::string*,
                           std::string*, int, int, const unsigned char*, int,
                           void*) { return true; };
    m_loader.SetImageLoader(image_loader, NULL);
  }

 private:
  bool Load(const char* _filename) override {
    bool success = false;
    std::string errors;
    std::string warnings;

    // Finds file extension.
    const char* separator = std::strrchr(_filename, '.');
    const char* ext = separator != NULL ? separator + 1 : "";

    // Tries to guess whether the input is a gltf json or a glb binary based on
    // the file extension
    if (std::strcmp(ext, "glb") == 0) {
      success =
          m_loader.LoadBinaryFromFile(&m_model, &errors, &warnings, _filename);
    } else {
      if (std::strcmp(ext, "gltf") != 0) {
        ozz::log::Log() << "Unknown file extension '" << ext
                        << "', assuming a JSON-formatted gltf." << std::endl;
      }

      success =
          m_loader.LoadASCIIFromFile(&m_model, &errors, &warnings, _filename);
    }

    // Prints any errors or warnings emitted by the loader
    if (!warnings.empty()) {
      ozz::log::Log() << "glTF parsing warnings: " << warnings << std::endl;
    }

    if (!errors.empty()) {
      ozz::log::Err() << "glTF parsing errors: " << errors << std::endl;
    }

    if (success) {
      ozz::log::Log() << "glTF parsed successfully." << std::endl;
    }

    if (success) {
      success &= FixupNames(m_model.scenes, "Scene", "scene_");
      success &= FixupNames(m_model.nodes, "Node", "node_");
      success &= FixupNames(m_model.animations, "Animation", "animation_");
    }

    return success;
  }

  // Given a skin find which of its joints is the skeleton root and return it
  // returns -1 if the skin has no associated joints
  int FindSkinRootJointIndex(const tinygltf::Skin& skin) {
    if (skin.joints.empty()) {
      return -1;
    }

    if (skin.skeleton != -1) {
      return skin.skeleton;
    }

    ozz::Map<int, int>::Std parents;
    for (int node : skin.joints) {
      for (int child : m_model.nodes[node].children) {
        parents[child] = node;
      }
    }

    int root = skin.joints[0];
    while (parents.find(root) != parents.end()) {
      root = parents[root];
    }

    return root;
  }

  bool Import(ozz::animation::offline::RawSkeleton* _skeleton,
              const NodeType& _types) override {
    (void)_types;

    if (m_model.scenes.empty()) {
      ozz::log::Err() << "No scenes found." << std::endl;
      return false;
    }

    // If no default scene has been set then take the first one spec does not
    // disallow gltfs without a default scene but it makes more sense to keep
    // going instead of throwing an error here
    int defaultScene = m_model.defaultScene;
    if (defaultScene == -1) {
      defaultScene = 0;
    }

    tinygltf::Scene& scene = m_model.scenes[defaultScene];
    ozz::log::LogV() << "Importing from default scene #" << defaultScene
                     << " with name \"" << scene.name << "\"." << std::endl;

    if (scene.nodes.empty()) {
      ozz::log::Err() << "Scene has no node." << std::endl;
      return false;
    }

    // Get all the skins belonging to this scene
    // TODO other than a set !!
    ozz::Set<int>::Std roots;
    ozz::Vector<tinygltf::Skin>::Std skins = GetSkinsForScene(scene);
    if (skins.empty()) {
      ozz::log::LogV() << "No skin exists in the scene, the whole scene graph "
                          "will be considered as a skeleton."
                       << std::endl;
      // Uses all scene nodes.
      for (auto& node : scene.nodes) {
        roots.insert(node);
      }
    } else {
      if (skins.size() > 1) {
        ozz::log::LogV() << "Multiple skins exist in the scene, they will all "
                            "be exported to a single skeleton."
                         << std::endl;
      }

      // Uses all skins root
      for (auto& skin : skins) {
        const int root = FindSkinRootJointIndex(skin);
        if (root == -1) {
          continue;
        }
        roots.insert(root);
      }
    }

    // Traverses the scene graph and record all joints starting from the roots.
    _skeleton->roots.resize(roots.size());
    int i = 0;  // TODO, better loop without a set
    for (int root : roots) {
      const tinygltf::Node& root_node = m_model.nodes[root];
      ozz::animation::offline::RawSkeleton::Joint& root_joint =
          _skeleton->roots[i++];

      if (!ImportNode(root_node, &root_joint)) {
        return false;
      }
    }

    if (!_skeleton->Validate()) {
      ozz::log::Err() << "Output skeleton failed validation. This is likely an "
                         "implementation issue."
                      << std::endl;
      return false;
    }

    return true;
  }

  // Recursively import a node's children
  bool ImportNode(const tinygltf::Node& _node,
                  ozz::animation::offline::RawSkeleton::Joint* _joint) {
    // Names joint.
    _joint->name = _node.name.c_str();

    // Fills transform.
    if (!CreateNodeTransform(_node, &_joint->transform)) {
      return false;
    }

    // Allocates all children at once.
    _joint->children.resize(_node.children.size());

    // Fills each child information.
    for (size_t i = 0; i < _node.children.size(); ++i) {
      const tinygltf::Node& child_node = m_model.nodes[_node.children[i]];
      ozz::animation::offline::RawSkeleton::Joint& child_joint =
          _joint->children[i];

      if (!ImportNode(child_node, &child_joint)) {
        return false;
      }
    }

    return true;
  }

  // Returns all animations in the gltf document.
  AnimationNames GetAnimationNames() override {
    AnimationNames animNames;
    for (size_t i = 0; i < m_model.animations.size(); ++i) {
      tinygltf::Animation& animation = m_model.animations[i];
      assert(animation.name.length() != 0);
      animNames.push_back(animation.name.c_str());
    }

    return animNames;
  }

  bool Import(const char* _animation_name,
              const ozz::animation::Skeleton& skeleton, float _sampling_rate,
              ozz::animation::offline::RawAnimation* _animation) override {
    if (_sampling_rate == 0.0f) {
      _sampling_rate = 30.0f;

      static bool samplingRateWarn = false;
      if (!samplingRateWarn) {
        ozz::log::LogV() << "The animation sampling rate is set to 0 "
                            "(automatic) but glTF does not carry scene frame "
                            "rate information. Assuming a sampling rate of "
                         << _sampling_rate << "hz." << std::endl;

        samplingRateWarn = true;
      }
    }

    // Find the corresponding gltf animation
    std::vector<tinygltf::Animation>::const_iterator gltf_animation =
        std::find_if(begin(m_model.animations), end(m_model.animations),
                     [_animation_name](const tinygltf::Animation& _animation) {
                       return _animation.name == _animation_name;
                     });
    assert(gltf_animation != end(m_model.animations));

    _animation->name = gltf_animation->name.c_str();

    // Animation duration is determined during sampling from the duration of the
    // longest channel
    _animation->duration = 0.0f;

    const int num_joints = skeleton.num_joints();
    _animation->tracks.resize(num_joints);

    // gltf stores animations by splitting them in channels
    // where each channel targets a node's property i.e. translation, rotation
    // or scale. ozz expects animations to be stored per joint so we create a
    // map where we record the associated channels for each joint
    ozz::CStringMap<std::vector<const tinygltf::AnimationChannel*>>::Std
        channels_per_joint;

    for (const tinygltf::AnimationChannel& channel : gltf_animation->channels) {
      if (channel.target_node == -1) {
        continue;
      }

      const tinygltf::Node& target_node = m_model.nodes[channel.target_node];
      channels_per_joint[target_node.name.c_str()].push_back(&channel);
    }

    // For each joint get all its associated channels, sample them and record
    // the samples in the joint track
    const ozz::Range<const char* const> joint_names = skeleton.joint_names();
    for (int i = 0; i < num_joints; i++) {
      auto& channels = channels_per_joint[joint_names[i]];
      auto& track = _animation->tracks[i];

      for (auto& channel : channels) {
        auto& sampler = gltf_animation->samplers[channel->sampler];
        if (!SampleAnimationChannel(m_model, sampler, channel->target_path,
                                    _sampling_rate, &_animation->duration,
                                    &track)) {
          return false;
        }
      }

      const tinygltf::Node* node = FindNodeByName(joint_names[i]);
      assert(node != nullptr);

      // Pads the bind pose transform for any joints which do not have an
      // associated channel for this animation
      if (track.translations.empty()) {
        track.translations.push_back(CreateTranslationBindPoseKey(*node));
      }
      if (track.rotations.empty()) {
        track.rotations.push_back(CreateRotationBindPoseKey(*node));
      }
      if (track.scales.empty()) {
        track.scales.push_back(CreateScaleBindPoseKey(*node));
      }
    }

    ozz::log::LogV() << "Processed animation '" << _animation->name
                     << "' (tracks: " << _animation->tracks.size()
                     << ", duration: " << _animation->duration << "s)."
                     << std::endl;

    if (!_animation->Validate()) {
      ozz::log::Err() << "Animation '" << _animation->name
                      << "' failed validation." << std::endl;
      return false;
    }

    return true;
  }

  bool SampleAnimationChannel(
      const tinygltf::Model& _model, const tinygltf::AnimationSampler& _sampler,
      const std::string& _target_path, float _sampling_rate, float* _duration,
      ozz::animation::offline::RawAnimation::JointTrack* _track) {
    auto& input = m_model.accessors[_sampler.input];
    assert(input.maxValues.size() == 1);

    // The max[0] property of the input accessor is the animation duration
    // this is required to be present by the spec:
    // "Animation Sampler's input accessor must have min and max properties
    // defined."
    const float duration = static_cast<float>(input.maxValues[0]);

    // If this channel's duration is larger than the animation's duration
    // then increase the animation duration to match
    if (duration > *_duration) {
      *_duration = duration;
    }

    assert(input.type == TINYGLTF_TYPE_SCALAR);
    auto& _output = m_model.accessors[_sampler.output];
    assert(_output.type == TINYGLTF_TYPE_VEC3 ||
           _output.type == TINYGLTF_TYPE_VEC4);

    const float* timestamps = BufferView<float>(_model, input);
    if (timestamps == nullptr) {
      return false;
    }

    if (_sampler.interpolation.empty()) {
      ozz::log::Err() << "Invalid sampler interpolation." << std::endl;
      return false;
    } else if (_sampler.interpolation == "LINEAR") {
      assert(input.count == _output.count);

      if (_target_path == "translation") {
        return SampleLinearChannel(m_model, _output, timestamps,
                                   &_track->translations);
      } else if (_target_path == "rotation") {
        return SampleLinearChannel(m_model, _output, timestamps,
                                   &_track->rotations);
      } else if (_target_path == "scale") {
        return SampleLinearChannel(m_model, _output, timestamps,
                                   &_track->scales);
      }
      ozz::log::Err() << "Invalid or unknown channel target path '"
                      << _target_path << "'." << std::endl;
      return false;
    } else if (_sampler.interpolation == "STEP") {
      assert(input.count == _output.count);

      if (_target_path == "translation") {
        return SampleStepChannel(m_model, _output, timestamps,
                                 &_track->translations);
      } else if (_target_path == "rotation") {
        return SampleStepChannel(m_model, _output, timestamps,
                                 &_track->rotations);
      } else if (_target_path == "scale") {
        return SampleStepChannel(m_model, _output, timestamps, &_track->scales);
      }
      ozz::log::Err() << "Invalid or unknown channel target path '"
                      << _target_path << "'." << std::endl;
      return false;
    } else if (_sampler.interpolation == "CUBICSPLINE") {
      assert(input.count * 3 == _output.count);

      if (_target_path == "translation") {
        return SampleCubicSplineChannel(m_model, _output, timestamps,
                                        &_track->translations, _sampling_rate,
                                        duration);
      } else if (_target_path == "rotation") {
        if (!SampleCubicSplineChannel(m_model, _output, timestamps,
                                      &_track->rotations, _sampling_rate,
                                      duration)) {
          return false;
        }

        // normalize all resulting quaternions per spec
        for (auto& key : _track->rotations) {
          key.value = ozz::math::Normalize(key.value);
        }

        return true;
      } else if (_target_path == "scale") {
        return SampleCubicSplineChannel(m_model, _output, timestamps,
                                        &_track->scales, _sampling_rate,
                                        duration);
      }
      ozz::log::Err() << "Invalid or unknown channel target path '"
                      << _target_path << "'." << std::endl;
      return false;
    }

    ozz::log::Err() << "Invalid or unknown interpolation type '"
                    << _sampler.interpolation << "'." << std::endl;
    return false;
  }

  // Returns all skins belonging to a given gltf scene
  ozz::Vector<tinygltf::Skin>::Std GetSkinsForScene(
      const tinygltf::Scene& _scene) const {
    ozz::Set<int>::Std open;
    ozz::Set<int>::Std found;

    for (int nodeIndex : _scene.nodes) {
      open.insert(nodeIndex);
    }

    while (!open.empty()) {
      int nodeIndex = *open.begin();
      found.insert(nodeIndex);
      open.erase(nodeIndex);

      auto& node = m_model.nodes[nodeIndex];
      for (int childIndex : node.children) {
        open.insert(childIndex);
      }
    }

    ozz::Vector<tinygltf::Skin>::Std skins;
    for (const tinygltf::Skin& skin : m_model.skins) {
      if (!skin.joints.empty() && found.find(skin.joints[0]) != found.end()) {
        skins.push_back(skin);
      }
    }

    return skins;
  }

  const tinygltf::Node* FindNodeByName(const std::string& _name) const {
    for (const tinygltf::Node& node : m_model.nodes) {
      if (node.name == _name) {
        return &node;
      }
    }

    return nullptr;
  }

  // no support for user-defined tracks
  NodeProperties GetNodeProperties(const char*) override {
    return NodeProperties();
  }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float,
              ozz::animation::offline::RawFloatTrack*) override {
    return false;
  }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float,
              ozz::animation::offline::RawFloat2Track*) override {
    return false;
  }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float,
              ozz::animation::offline::RawFloat3Track*) override {
    return false;
  }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float,
              ozz::animation::offline::RawFloat4Track*) override {
    return false;
  }

  tinygltf::TinyGLTF m_loader;
  tinygltf::Model m_model;
};

int main(int _argc, const char** _argv) {
  GltfImporter converter;
  return converter(_argc, _argv);
}
