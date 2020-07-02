// ======================================================================== //
// Copyright 2019-2020 Ingo Wald                                            //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "Device.h"
#include "owl/common/parallel/parallel_for.h"

#define LOG(message)      \
  if (Context::logging()) \
  std::cout << "#owl.ll(" << context->owlDeviceID << "): "      \
  << message                                                    \
  << std::endl

#define LOG_OK(message)                                 \
  if (Context::logging()) \
  std::cout << OWL_TERMINAL_GREEN                       \
  << "#owl.ll(" << context->owlDeviceID << "): "        \
  << message << OWL_TERMINAL_DEFAULT << std::endl

#define CLOG(message)                                   \
  if (Context::logging()) \
  std::cout << "#owl.ll(" << owlDeviceID << "): "       \
  << message                                            \
  << std::endl

#define CLOG_OK(message)                                \
  if (Context::logging()) \
  std::cout << OWL_TERMINAL_GREEN                       \
  << "#owl.ll(" << owlDeviceID << "): "                 \
  << message << OWL_TERMINAL_DEFAULT << std::endl

namespace owl {
  namespace ll {

    /*! set given child to {childGroupID+xfm}  */
    void Device::instanceGroupSetChild(int groupID,
                                       int childNo,
                                       int childGroupID)
    {
      InstanceGroup *ig = checkGetInstanceGroup(groupID);
      Group *newChild = checkGetGroup(childGroupID);
      Group *oldChild = ig->children[childNo];
      if (oldChild)
        oldChild->numTimesReferenced--;
      ig->children[childNo] = newChild;
      newChild->numTimesReferenced++;
    }

    void Device::instanceGroupCreate(/*! the group we are defining */
                                     int groupID,
                                     size_t childCount,
                                     /* list of children. list can be
                                        omitted by passing a nullptr, but if
                                        not null this must be a list of
                                        'childCount' valid group ID */
                                     const uint32_t *childGroupIDs)
    {
      assert("check for valid ID" && groupID >= 0);
      assert("check for valid ID" && groupID < groups.size());
      assert("check group ID is available" && groups[groupID] == nullptr);
        
      InstanceGroup *group
        = new InstanceGroup(childCount);
      assert("check 'new' was successful" && group != nullptr);
      groups[groupID] = group;

      owl::parallel_for
        (childCount,[&](size_t childID){
          if (childGroupIDs) {
            int childGroupID = childGroupIDs[childID];
            assert("check geom child child group ID is valid"
                   && childGroupID >= 0);
            assert("check geom child child group ID is valid"
                   && childGroupID <  groups.size());
            Group *childGroup = groups[childGroupID];
            assert("check referened child groups is valid" && childGroup != nullptr);
            childGroup->numTimesReferenced++;
            group->children[childID] = childGroup;
          }
        },8*1024);
    }

    void InstanceGroup::destroyAccel(Context *context) 
    {
      int oldActive = context->pushActive();
      if (traversable) {
        bvhMemory.free();
        traversable = 0;
      }
      context->popActive(oldActive);
    }
    
    void InstanceGroup::buildAccel(Context *context)
    {
      if (transforms[1] == nullptr)
        buildOrRefit_staticInstances<true>(context);
      else
        buildOrRefit_motionBlur<true>(context);
    }

    void InstanceGroup::refitAccel(Context *context)
    {
      if (transforms[1] == nullptr)
        buildOrRefit_staticInstances<false>(context);
      else
        buildOrRefit_motionBlur<false>(context);
    }

    template<bool FULL_REBUILD>
    void InstanceGroup::buildOrRefit_staticInstances(Context *context) 
    {
      if (FULL_REBUILD) {
        assert("check does not yet exist" && traversable == 0);
        assert("check does not yet exist" && bvhMemory.empty());
      } else {
        assert("check does not yet exist" && traversable != 0);
        assert("check does not yet exist" && !bvhMemory.empty());
      }
      
      int oldActive = context->pushActive();
      LOG("building instance accel over "
          << children.size() << " groups");

      // ==================================================================
      // sanity check that that many instances are actualy allowed by optix:
      // ==================================================================
      uint32_t maxInstsPerIAS = 0;
      optixDeviceContextGetProperty
        (context->optixContext,
         OPTIX_DEVICE_PROPERTY_LIMIT_MAX_INSTANCES_PER_IAS,
         &maxInstsPerIAS,
         sizeof(maxInstsPerIAS));
      
      if (children.size() > maxInstsPerIAS)
        throw std::runtime_error("number of children in instance group exceeds "
                                 "OptiX's MAX_INSTANCES_PER_IAS limit");
      
      // ==================================================================
      // create instance build inputs
      // ==================================================================
      OptixBuildInput              instanceInput  {};
      OptixAccelBuildOptions       accelOptions   {};
      //! the N build inputs that go into the builder
      std::vector<OptixBuildInput> buildInputs(children.size());
      std::vector<OptixInstance>   optixInstances(children.size());

     // for now we use the same flags for all geoms
      uint32_t instanceGroupInputFlags[1] = { 0 };
      // { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT

      // now go over all children to set up the buildinputs
      for (int childID=0;childID<children.size();childID++) {
        Group *child = children[childID];
        assert(child);

        assert(transforms[1] == nullptr);
        const affine3f xfm = transforms[0][childID];

        OptixInstance &oi    = optixInstances[childID];
        oi.transform[0*4+0]  = xfm.l.vx.x;
        oi.transform[0*4+1]  = xfm.l.vy.x;
        oi.transform[0*4+2]  = xfm.l.vz.x;
        oi.transform[0*4+3]  = xfm.p.x;
        
        oi.transform[1*4+0]  = xfm.l.vx.y;
        oi.transform[1*4+1]  = xfm.l.vy.y;
        oi.transform[1*4+2]  = xfm.l.vz.y;
        oi.transform[1*4+3]  = xfm.p.y;
        
        oi.transform[2*4+0]  = xfm.l.vx.z;
        oi.transform[2*4+1]  = xfm.l.vy.z;
        oi.transform[2*4+2]  = xfm.l.vz.z;
        oi.transform[2*4+3]  = xfm.p.z;
        
        oi.flags             = OPTIX_INSTANCE_FLAG_NONE;
        oi.instanceId        = (instanceIDs==nullptr)?childID:instanceIDs[childID];
        oi.visibilityMask    = 255;
        oi.sbtOffset         = context->numRayTypes * child->getSBTOffset();
        oi.visibilityMask    = 255;
        assert(child->traversable);
        oi.traversableHandle = child->traversable;
      }

      optixInstanceBuffer.alloc(optixInstances.size()*
                                sizeof(optixInstances[0]));
      optixInstanceBuffer.upload(optixInstances.data(),"optixinstances");
    
      // ==================================================================
      // set up build input
      // ==================================================================
      instanceInput.type
        = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
      instanceInput.instanceArray.instances
        = (CUdeviceptr)optixInstanceBuffer.get();
      instanceInput.instanceArray.numInstances
        = (int)optixInstances.size();
      
      // ==================================================================
      // set up accel uptions
      // ==================================================================
      accelOptions.buildFlags =
        OPTIX_BUILD_FLAG_PREFER_FAST_TRACE
        |
        OPTIX_BUILD_FLAG_ALLOW_UPDATE
        ;
      accelOptions.motionOptions.numKeys = 1;
      if (FULL_REBUILD)
        accelOptions.operation            = OPTIX_BUILD_OPERATION_BUILD;
      else
        accelOptions.operation            = OPTIX_BUILD_OPERATION_UPDATE;
      
      // ==================================================================
      // query build buffer sizes, and allocate those buffers
      // ==================================================================
      OptixAccelBufferSizes blasBufferSizes;
      OPTIX_CHECK(optixAccelComputeMemoryUsage(context->optixContext,
                                               &accelOptions,
                                               &instanceInput,
                                               1, // num build inputs
                                               &blasBufferSizes
                                               ));
    
      // ==================================================================
      // trigger the build ....
      // ==================================================================
      const size_t tempSize
        = FULL_REBUILD
        ? blasBufferSizes.tempSizeInBytes
        : blasBufferSizes.tempUpdateSizeInBytes;
      LOG("starting to build/refit "
          << prettyNumber(optixInstances.size()) << " instances, "
          << prettyNumber(blasBufferSizes.outputSizeInBytes) << "B in output and "
          << prettyNumber(tempSize) << "B in temp data");
      
      DeviceMemory tempBuffer;
      tempBuffer.alloc(tempSize);
      
      if (FULL_REBUILD)
        bvhMemory.alloc(blasBufferSizes.outputSizeInBytes);
      
      OPTIX_CHECK(optixAccelBuild(context->optixContext,
                                  /* todo: stream */0,
                                  &accelOptions,
                                  // array of build inputs:
                                  &instanceInput,1,
                                  // buffer of temp memory:
                                  (CUdeviceptr)tempBuffer.get(),
                                  tempBuffer.size(),
                                  // where we store initial, uncomp bvh:
                                  (CUdeviceptr)bvhMemory.get(),
                                  bvhMemory.size(),
                                  /* the traversable we're building: */ 
                                  &traversable,
                                  /* no compaction for instances: */
                                  nullptr,0u
                                  ));
      
      CUDA_SYNC_CHECK();
    
      // ==================================================================
      // aaaaaand .... clean up
      // ==================================================================
      // TODO: move those free's to the destructor, so we can delay the
      // frees until all objects are done
      tempBuffer.free();
      context->popActive(oldActive);
      
      LOG_OK("successfully built instance group accel");
    }
    





    template<bool FULL_REBUILD>
    void InstanceGroup::buildOrRefit_motionBlur(Context *context) 
    {
      if (FULL_REBUILD) {
        assert("check does not yet exist" && traversable == 0);
        assert("check does not yet exist" && bvhMemory.empty());
      } else {
        assert("check does not yet exist" && traversable != 0);
        assert("check does not yet exist" && !bvhMemory.empty());
      }
      
      int oldActive = context->pushActive();
      LOG("building instance accel over "
          << children.size() << " groups");

      // ==================================================================
      // sanity check that that many instances are actualy allowed by optix:
      // ==================================================================
      uint32_t maxInstsPerIAS = 0;
      optixDeviceContextGetProperty
        (context->optixContext,
         OPTIX_DEVICE_PROPERTY_LIMIT_MAX_INSTANCES_PER_IAS,
         &maxInstsPerIAS,
         sizeof(maxInstsPerIAS));
      
      if (children.size() > maxInstsPerIAS)
        throw std::runtime_error("number of children in instnace group exceeds "
                                 "OptiX's MAX_INSTANCES_PER_IAS limit");
      
      // ==================================================================
      // build motion transforms
      // ==================================================================
      assert(transforms[1] != nullptr);
      std::vector<OptixMatrixMotionTransform> motionTransforms(children.size());
      for (int childID=0;childID<children.size();childID++) {
        Group *child = children[childID];
        assert(child);
        OptixMatrixMotionTransform &mt = motionTransforms[childID];
        mt.child                      = child->traversable;//state.sphere_gas_handle;
        mt.motionOptions.numKeys      = 2;
        mt.motionOptions.timeBegin    = 0.0f;
        mt.motionOptions.timeEnd      = 1.0f;
        mt.motionOptions.flags        = OPTIX_MOTION_FLAG_NONE;
        for (int timeStep = 0; timeStep < 2; timeStep ++ ) {
          const affine3f xfm = transforms[timeStep][childID];
          mt.transform[timeStep][0*4+0]  = xfm.l.vx.x;
          mt.transform[timeStep][0*4+1]  = xfm.l.vy.x;
          mt.transform[timeStep][0*4+2]  = xfm.l.vz.x;
          mt.transform[timeStep][0*4+3]  = xfm.p.x;
          
          mt.transform[timeStep][1*4+0]  = xfm.l.vx.y;
          mt.transform[timeStep][1*4+1]  = xfm.l.vy.y;
          mt.transform[timeStep][1*4+2]  = xfm.l.vz.y;
          mt.transform[timeStep][1*4+3]  = xfm.p.y;
          
          mt.transform[timeStep][2*4+0]  = xfm.l.vx.z;
          mt.transform[timeStep][2*4+1]  = xfm.l.vy.z;
          mt.transform[timeStep][2*4+2]  = xfm.l.vz.z;
          mt.transform[timeStep][2*4+3]  = xfm.p.z;
        }
      }
      // and upload
      motionTransformsBuffer.alloc(motionTransforms.size()*
                                   sizeof(motionTransforms[0]));
      motionTransformsBuffer.upload(motionTransforms.data(),"motionTransforms");
      
      // ==================================================================
      // create instance build inputs
      // ==================================================================
      OptixBuildInput              instanceInput  {};
      OptixAccelBuildOptions       accelOptions   {};
      //! the N build inputs that go into the builder
      std::vector<OptixBuildInput> buildInputs(children.size());
      std::vector<OptixInstance>   optixInstances(children.size());

     // for now we use the same flags for all geoms
      uint32_t instanceGroupInputFlags[1] = { 0 };
      // { OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT

      // now go over all children to set up the buildinputs
      for (int childID=0;childID<children.size();childID++) {
        Group *child = children[childID];
        assert(child);

        OptixTraversableHandle childMotionHandle = 0;
        OPTIX_CHECK(optixConvertPointerToTraversableHandle
                    (context->optixContext,
                     (CUdeviceptr)(((const uint8_t*)motionTransformsBuffer.get())
                                   +childID*sizeof(motionTransforms[0])),
                     OPTIX_TRAVERSABLE_TYPE_MATRIX_MOTION_TRANSFORM,
                     &childMotionHandle));
        
        OptixInstance &oi    = optixInstances[childID];
        oi.transform[0*4+0]  = 1.f;//xfm.l.vx.x;
        oi.transform[0*4+1]  = 0.f;//xfm.l.vy.x;
        oi.transform[0*4+2]  = 0.f;//xfm.l.vz.x;
        oi.transform[0*4+3]  = 0.f;//xfm.p.x;
        
        oi.transform[1*4+0]  = 0.f;//xfm.l.vx.y;
        oi.transform[1*4+1]  = 1.f;//xfm.l.vy.y;
        oi.transform[1*4+2]  = 0.f;//xfm.l.vz.y;
        oi.transform[1*4+3]  = 0.f;//xfm.p.y;
        
        oi.transform[2*4+0]  = 0.f;//xfm.l.vx.z;
        oi.transform[2*4+1]  = 0.f;//xfm.l.vy.z;
        oi.transform[2*4+2]  = 1.f;//xfm.l.vz.z;
        oi.transform[2*4+3]  = 0.f;//xfm.p.z;
        
        oi.flags             = OPTIX_INSTANCE_FLAG_NONE;
        oi.instanceId        = (instanceIDs==nullptr)?childID:instanceIDs[childID];
        oi.visibilityMask    = 255;
        oi.sbtOffset         = context->numRayTypes * child->getSBTOffset();
        oi.visibilityMask    = 255;
        assert(child->traversable);
        oi.traversableHandle = child->traversable;
      }

      optixInstanceBuffer.alloc(optixInstances.size()*
                                sizeof(optixInstances[0]));
      optixInstanceBuffer.upload(optixInstances.data(),"optixinstances");
    
      // ==================================================================
      // set up build input
      // ==================================================================
      instanceInput.type
        = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
      instanceInput.instanceArray.instances
        = (CUdeviceptr)optixInstanceBuffer.get();
      instanceInput.instanceArray.numInstances
        = (int)optixInstances.size();
      
      // ==================================================================
      // set up accel uptions
      // ==================================================================
      accelOptions.buildFlags =
        OPTIX_BUILD_FLAG_PREFER_FAST_TRACE
        |
        OPTIX_BUILD_FLAG_ALLOW_UPDATE
        ;
      accelOptions.motionOptions.numKeys = 1;
      if (FULL_REBUILD)
        accelOptions.operation            = OPTIX_BUILD_OPERATION_BUILD;
      else
        accelOptions.operation            = OPTIX_BUILD_OPERATION_UPDATE;
      
      // ==================================================================
      // query build buffer sizes, and allocate those buffers
      // ==================================================================
      OptixAccelBufferSizes blasBufferSizes;
      OPTIX_CHECK(optixAccelComputeMemoryUsage(context->optixContext,
                                               &accelOptions,
                                               &instanceInput,
                                               1, // num build inputs
                                               &blasBufferSizes
                                               ));
    
      // ==================================================================
      // trigger the build ....
      // ==================================================================
      const size_t tempSize
        = FULL_REBUILD
        ? blasBufferSizes.tempSizeInBytes
        : blasBufferSizes.tempUpdateSizeInBytes;
      LOG("starting to build/refit "
          << prettyNumber(optixInstances.size()) << " instances, "
          << prettyNumber(blasBufferSizes.outputSizeInBytes) << "B in output and "
          << prettyNumber(tempSize) << "B in temp data");
      
      DeviceMemory tempBuffer;
      tempBuffer.alloc(tempSize);
      
      if (FULL_REBUILD)
        bvhMemory.alloc(blasBufferSizes.outputSizeInBytes);
      
      OPTIX_CHECK(optixAccelBuild(context->optixContext,
                                  /* todo: stream */0,
                                  &accelOptions,
                                  // array of build inputs:
                                  &instanceInput,1,
                                  // buffer of temp memory:
                                  (CUdeviceptr)tempBuffer.get(),
                                  tempBuffer.size(),
                                  // where we store initial, uncomp bvh:
                                  (CUdeviceptr)bvhMemory.get(),
                                  bvhMemory.size(),
                                  /* the traversable we're building: */ 
                                  &traversable,
                                  /* no compaction for instances: */
                                  nullptr,0u
                                  ));
      
      CUDA_SYNC_CHECK();
    
      // ==================================================================
      // aaaaaand .... clean up
      // ==================================================================
      // TODO: move those free's to the destructor, so we can delay the
      // frees until all objects are done
      tempBuffer.free();
      context->popActive(oldActive);
      
      LOG_OK("successfully built instance group accel");
    }
    

  } // ::owl::ll
} //::owl
