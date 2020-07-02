// ======================================================================== //
// Copyright 2019 Ingo Wald                                                 //
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

#pragma once

#include "RegisteredObject.h"
#include "Geometry.h"

namespace owl {

  struct Group : public RegisteredObject {
    typedef std::shared_ptr<Group> SP;
    
    Group(Context *const context,
          ObjectRegistry &registry)
      : RegisteredObject(context,registry)
    {}
    virtual std::string toString() const { return "Group"; }
    virtual void buildAccel();
    virtual void refitAccel();

    OptixTraversableHandle getTraversable(int deviceID);
  };

  
  struct GeomGroup : public Group {
    typedef std::shared_ptr<GeomGroup> SP;

    GeomGroup(Context *const context,
              size_t numChildren);
    void setChild(int childID, Geom::SP child);
    
    virtual std::string toString() const { return "GeomGroup"; }
    std::vector<Geom::SP> geometries;
  };

  struct TrianglesGeomGroup : public GeomGroup {
    TrianglesGeomGroup(Context *const context,
                   size_t numChildren);
    virtual std::string toString() const { return "TrianglesGeomGroup"; }
  };

  struct UserGeomGroup : public GeomGroup {
    UserGeomGroup(Context *const context,
                   size_t numChildren);
    virtual std::string toString() const { return "UserGeomGroup"; }

    /*! build() and refit() share most of their code; this functoin
        does all that code, with only minor specialization based on
        build vs refit */
    void buildOrRefit(bool FULL_REBUILD);
    
    void buildAccel() override;
    void refitAccel() override;
  };

} // ::owl
