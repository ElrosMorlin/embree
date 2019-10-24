// ======================================================================== //
// Copyright 2009-2019 Intel Corporation                                    //
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

#include "../common/scene.h"
#include "../common/primref.h"
#include "../common/primref_mb.h"
#include "priminfo.h"
#include "bvh_builder_morton.h"
#include "../builders/splitter.h"

#include "../../common/algorithms/parallel_for_for.h"
#include "../../common/algorithms/parallel_for_for_prefix_sum.h"

#define GRID_SIZE 1024

#define DBG_PRESPLIT(x) 
#define MAX_PRESPLITS_PER_PRIMITIVE_LOG 5
#define MAX_PRESPLITS_PER_PRIMITIVE (1<<MAX_PRESPLITS_PER_PRIMITIVE_LOG)

#define PRIORITY_CUTOFF_THRESHOLD 1.0f
#define PRIORITY_BINARY_SEACH_THRESHOLD 1.0f
#define NEW_PRIO 1

namespace embree
{  
  namespace isa
  {

    struct PresplitItem
    {
      union {
        float priority;    
        unsigned int data;
      };
      unsigned int index;

      __forceinline operator unsigned() const
      {
	return reinterpret_cast<const unsigned&>(priority);
      }
      __forceinline bool operator < (const PresplitItem& item) const
      {
	return (priority < item.priority);
      }

      template<typename Mesh>
      __forceinline static float compute_priority(const PrimRef &ref, Scene *scene, const Vec2i &mc)
      {
	const unsigned int geomID = ref.geomID();
	const unsigned int primID = ref.primID();
	const float area_aabb  = area(ref.bounds());
	const float area_prim  = ((Mesh*)scene->get(geomID))->projectedPrimitiveArea(primID);
#if NEW_PRIO == 1
        const unsigned int diff = 31 - lzcnt(mc.x^mc.y);
        const float priority = powf((area_aabb - 2.0f*area_prim) * powf(1.5f,(float)diff),1.0f/4.0f);      
	return priority;      
#else
        const float priority = area_aabb;
	const float area_ratio = min(4.0f, area_aabb / max(1E-12f,area_prim));
	return priority * area_ratio;      
#endif
      }

    
    };

    inline std::ostream &operator<<(std::ostream &cout, const PresplitItem& item) {
      return cout << "index " << item.index << " priority " << item.priority;    
    };

    template<typename SplitterFactory>    
    void splitPrimitive(SplitterFactory &Splitter,
                        const PrimRef &prim,
                        const unsigned int geomID,
                        const unsigned int primID,
                        const unsigned int split_level,
                        const Vec3fa &grid_base, 
                        const float grid_scale,
                        const float grid_extend,
                        PrimRef subPrims[MAX_PRESPLITS_PER_PRIMITIVE],
                        size_t &numSubPrims)
    {
      assert(split_level <= MAX_PRESPLITS_PER_PRIMITIVE_LOG);
      if (split_level == 0)
      {
        assert(numSubPrims < MAX_PRESPLITS_PER_PRIMITIVE);
        subPrims[numSubPrims++] = prim;
      }
      else
      {
        const Vec3fa lower = prim.lower;
        const Vec3fa upper = prim.upper;
        const Vec3fa glower = (lower-grid_base)*Vec3fa(grid_scale)+Vec3fa(0.2f);
        const Vec3fa gupper = (upper-grid_base)*Vec3fa(grid_scale)-Vec3fa(0.2f);
        Vec3ia ilower(floor(glower));
        Vec3ia iupper(floor(gupper));

        /* this ignores dimensions that are empty */
        iupper = (__m128i)select(vint4(glower) >= vint4(gupper),vint4(ilower),vint4(iupper));

        /* compute a morton code for the lower and upper grid coordinates. */
        const unsigned int lower_code = bitInterleave(ilower.x,ilower.y,ilower.z);
        const unsigned int upper_code = bitInterleave(iupper.x,iupper.y,iupper.z);
			
        /* if all bits are equal then we cannot split */
        if(unlikely(lower_code == upper_code))
        {
          assert(numSubPrims < MAX_PRESPLITS_PER_PRIMITIVE);
          subPrims[numSubPrims++] = prim;
          return;
        }

        const unsigned int diff = 31 - lzcnt(lower_code^upper_code);
		    
        /* compute octree level and dimension to perform the split in */
        const unsigned int level = diff / 3;
        const unsigned int dim   = diff % 3;
      
        /* now we compute the grid position of the split */
        const unsigned int isplit = iupper[dim] & ~((1<<level)-1);
			    
        /* compute world space position of split */
        const float inv_grid_size = 1.0f / GRID_SIZE;
        const float fsplit = grid_base[dim] + isplit * inv_grid_size * grid_extend;

        assert(prim.lower[dim] <= fsplit &&
               prim.upper[dim] >= fsplit);
		
        /* split primitive */
        const auto splitter = Splitter(prim);
        BBox3fa left,right;
        splitter(prim.bounds(),dim,fsplit,left,right);
        assert(!left.empty());
        assert(!right.empty());

			    
        splitPrimitive(Splitter,PrimRef(left ,geomID,primID),geomID,primID,split_level-1,grid_base,grid_scale,grid_extend,subPrims,numSubPrims);
        splitPrimitive(Splitter,PrimRef(right,geomID,primID),geomID,primID,split_level-1,grid_base,grid_scale,grid_extend,subPrims,numSubPrims);
      }
    }
    
    
    template<typename Mesh, typename SplitterFactory>    
      PrimInfo createPrimRefArray_presplit(Geometry* geometry, size_t numPrimRefs, mvector<PrimRef>& prims, BuildProgressMonitor& progressMonitor)
    {
      ParallelPrefixSumState<PrimInfo> pstate;
      
      /* first try */
      progressMonitor(0);
      PrimInfo pinfo = parallel_prefix_sum( pstate, size_t(0), geometry->size(), size_t(1024), PrimInfo(empty), [&](const range<size_t>& r, const PrimInfo& base) -> PrimInfo {
	  return geometry->createPrimRefArray(prims,r,r.begin());
	}, [](const PrimInfo& a, const PrimInfo& b) -> PrimInfo { return PrimInfo::merge(a,b); });

      /* if we need to filter out geometry, run again */
      if (pinfo.size() != numPrimRefs)
	{
	  progressMonitor(0);
	  pinfo = parallel_prefix_sum( pstate, size_t(0), geometry->size(), size_t(1024), PrimInfo(empty), [&](const range<size_t>& r, const PrimInfo& base) -> PrimInfo {
	      return geometry->createPrimRefArray(prims,r,base.size());
	    }, [](const PrimInfo& a, const PrimInfo& b) -> PrimInfo { return PrimInfo::merge(a,b); });
	}
      return pinfo;	
    }
    
    __forceinline Vec2i computeMC(const Vec3fa &grid_base, const float grid_scale, const PrimRef &ref)
    {
      const Vec3fa lower = ref.lower;
      const Vec3fa upper = ref.upper;
      const Vec3fa glower = (lower-grid_base)*Vec3fa(grid_scale)+Vec3fa(0.2f);
      const Vec3fa gupper = (upper-grid_base)*Vec3fa(grid_scale)-Vec3fa(0.2f);
      Vec3ia ilower(floor(glower));
      Vec3ia iupper(floor(gupper));
      
      /* this ignores dimensions that are empty */
      iupper = (__m128i)select(vint4(glower) >= vint4(gupper),vint4(ilower),vint4(iupper));

      /* compute a morton code for the lower and upper grid coordinates. */
      const unsigned int lower_code = bitInterleave(ilower.x,ilower.y,ilower.z);
      const unsigned int upper_code = bitInterleave(iupper.x,iupper.y,iupper.z);
      return Vec2i(lower_code,upper_code);
    }

    template<typename Mesh, typename SplitterFactory>    
      PrimInfo createPrimRefArray_presplit(Scene* scene, Geometry::GTypeMask types, bool mblur, size_t numPrimRefs, mvector<PrimRef>& prims, BuildProgressMonitor& progressMonitor)
    {	
      ParallelForForPrefixSumState<PrimInfo> pstate;
      Scene::Iterator2 iter(scene,types,mblur);
      
      /* first try */
      progressMonitor(0);
      pstate.init(iter,size_t(1024));
      PrimInfo pinfo = parallel_for_for_prefix_sum0( pstate, iter, PrimInfo(empty), [&](Geometry* mesh, const range<size_t>& r, size_t k) -> PrimInfo {
	  return mesh->createPrimRefArray(prims,r,k);
	}, [](const PrimInfo& a, const PrimInfo& b) -> PrimInfo { return PrimInfo::merge(a,b); });
      
      /* if we need to filter out geometry, run again */
      if (pinfo.size() != numPrimRefs)
	{
	  progressMonitor(0);
	  pinfo = parallel_for_for_prefix_sum1( pstate, iter, PrimInfo(empty), [&](Geometry* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo {
	      return mesh->createPrimRefArray(prims,r,base.size());
	    }, [](const PrimInfo& a, const PrimInfo& b) -> PrimInfo { return PrimInfo::merge(a,b); });
	}

      /* use correct number of primitives */
      size_t numPrimitives = pinfo.size();
      const size_t alloc_numPrimitives = prims.size(); 
      const size_t numPrimitivesToSplit = alloc_numPrimitives - numPrimitives;

      /* set up primitive splitter */
      SplitterFactory Splitter(scene);


      DBG_PRESPLIT(
        const size_t org_numPrimitives = pinfo.size();
        PRINT(numPrimitives);		
        PRINT(alloc_numPrimitives);		
        PRINT(numPrimitivesToSplit);
        );

      /* allocate double buffer presplit items */
      PresplitItem *presplitItem     = (PresplitItem*)alignedMalloc(sizeof(PresplitItem)*alloc_numPrimitives,64);
      PresplitItem *tmp_presplitItem = (PresplitItem*)alignedMalloc(sizeof(PresplitItem)*alloc_numPrimitives,64);

      /* compute grid */
      const Vec3fa grid_base    = pinfo.geomBounds.lower;
      const Vec3fa grid_diag    = pinfo.geomBounds.size();
      const float grid_extend   = max(grid_diag.x,max(grid_diag.y,grid_diag.z));		
      const float grid_scale    = grid_extend == 0.0f ? 0.0f : GRID_SIZE / grid_extend;

      /* init presplit items and get total sum */
      const float psum = parallel_reduce( size_t(0), numPrimitives, 0.0f, [&](const range<size_t>& r) -> float {
          float sum = 0.0f;
	  for (size_t i=r.begin(); i<r.end(); i++)
	    {		
	      presplitItem[i].index = (unsigned int)i;
              const Vec2i mc = computeMC(grid_base,grid_scale,prims[i]);
              /* if all bits are equal then we cannot split */
              presplitItem[i].priority = (mc.x != mc.y) ? PresplitItem::compute_priority<Mesh>(prims[i],scene,mc) : 0.0f;    
              /* FIXME: sum undeterministic */
              sum += presplitItem[i].priority;
	    }
          return sum;
	},[](const float& a, const float& b) -> float { return a+b; });

      /* compute number of splits per primitive */
      const float inv_psum = 1.0f / psum;
      parallel_for( size_t(0), numPrimitives, size_t(1024), [&](const range<size_t>& r) -> void {
	  for (size_t i=r.begin(); i<r.end(); i++)
            if (presplitItem[i].priority > 0.0f)
            {
              const float rel_p = (float)numPrimitivesToSplit * presplitItem[i].priority * inv_psum;
              if (rel_p >= PRIORITY_CUTOFF_THRESHOLD) // need at least a split budget that generates two sub-prims
              {
                presplitItem[i].priority = max(min(ceilf(logf(rel_p)/logf(2.0f)),(float)MAX_PRESPLITS_PER_PRIMITIVE_LOG),1.0f);
                assert(presplitItem[i].priority >= 1.0f && presplitItem[i].priority <= (float)MAX_PRESPLITS_PER_PRIMITIVE_LOG);
              }
              else
                presplitItem[i].priority = 0.0f;
            }
	});

      /* sort presplit items in ascending order */
      radix_sort_u32(presplitItem,tmp_presplitItem,numPrimitives,1024);

      DBG_PRESPLIT(
        for (size_t i=1;i<numPrimitives;i++)
          assert(presplitItem[i-1].priority <= presplitItem[i].priority);
        );

      /* binary search to find index with priority >= 1.0f */
      size_t l = 0;
      size_t r = numPrimitives;		    
      while(l+1 < r)
      {
        const size_t mid = (l+r)/2;
        if (presplitItem[mid].priority < PRIORITY_BINARY_SEACH_THRESHOLD) 
          l = mid;
        else
          r = mid;
      }

      DBG_PRESPLIT(
        PRINT(psum / numPrimitives);
        PRINT(numPrimitives);
        PRINT(r);
        );

      if (r < numPrimitives)
      {
        assert(presplitItem[r].priority >= 1.0f);

        /* compute actual number of sub-primitives generated within the [r;numPrimitives-1] range */
        const size_t totalNumSubPrims = parallel_reduce( size_t(r), numPrimitives, size_t(0), [&](const range<size_t>& t) -> size_t {
            size_t sum = 0;
            for (size_t i=t.begin(); i<t.end(); i++)
	    {	
              PrimRef subPrims[MAX_PRESPLITS_PER_PRIMITIVE];	
              assert(presplitItem[i].priority >= 1.0f);
              const unsigned int  primrefID = presplitItem[i].index;	
              const float prio              = presplitItem[i].priority;
              const unsigned int   geomID   = prims[primrefID].geomID();
              const unsigned int   primID   = prims[primrefID].primID();
              const unsigned int split_levels = (unsigned int)prio;
              size_t numSubPrims = 0;
              splitPrimitive(Splitter,prims[primrefID],geomID,primID,split_levels,grid_base,grid_scale,grid_extend,subPrims,numSubPrims);
              assert(numSubPrims);
              numSubPrims--; // can reuse slot 
              sum+=numSubPrims;
              presplitItem[i].data = (numSubPrims << MAX_PRESPLITS_PER_PRIMITIVE_LOG) | split_levels;
	    }
            return sum;
          },[](const size_t& a, const size_t& b) -> size_t { return a+b; });

        /* if we are over budget, need to shrink the range */
        if (totalNumSubPrims > numPrimitivesToSplit) 
        {
          size_t new_r = numPrimitives-1;
          size_t sum = 0;
          for (;new_r>=r;new_r--)
          {
            const unsigned int numSubPrims = presplitItem[new_r].data >> MAX_PRESPLITS_PER_PRIMITIVE_LOG;
            if (unlikely(sum + numSubPrims >= numPrimitivesToSplit)) break;
            sum += numSubPrims;
          }
          new_r++;
          r = new_r;
        }


        /* iterate over range, and split primitives into sub primitives and append them to prims array */		    
        __aligned(64) std::atomic<size_t> offset;
        offset.store(0);
        parallel_for( size_t(r), numPrimitives, size_t(128), [&](const range<size_t>& r) -> void {
            for (size_t j=r.begin(); j<r.end(); j++)		    
            {
              PrimRef subPrims[MAX_PRESPLITS_PER_PRIMITIVE];
              const unsigned int  primrefID = presplitItem[j].index;	
              const unsigned int   geomID   = prims[primrefID].geomID();
              const unsigned int   primID   = prims[primrefID].primID();
              const unsigned int split_levels = presplitItem[j].data & ((unsigned int)(1 << MAX_PRESPLITS_PER_PRIMITIVE_LOG)-1);

              assert(split_levels);
              assert(split_levels <= MAX_PRESPLITS_PER_PRIMITIVE_LOG);
              size_t numSubPrims = 0;
              splitPrimitive(Splitter,prims[primrefID],geomID,primID,split_levels,grid_base,grid_scale,grid_extend,subPrims,numSubPrims);

              const size_t newID = numPrimitives + offset.fetch_add(numSubPrims-1);
              assert(newID+numSubPrims <= alloc_numPrimitives);
              prims[primrefID] = subPrims[0];
              for (size_t i=1;i<numSubPrims;i++)
                prims[newID+i-1] = subPrims[i];
            }
          });

        numPrimitives += offset;
        DBG_PRESPLIT(
          PRINT(numPrimitives);
          PRINT((float)numPrimitives/org_numPrimitives);
          );
      }

      /* recompute centroid bounding boxes */
      pinfo = parallel_reduce(size_t(0),numPrimitives,PrimInfo(empty), [&] (const range<size_t>& r) -> PrimInfo {
	  PrimInfo p(empty);
	  for (size_t j=r.begin(); j<r.end(); j++)
	    p.add_center2(prims[j]);
	  return p;
	}, [](const PrimInfo& a, const PrimInfo& b) -> PrimInfo { return PrimInfo::merge(a,b); });
  
      assert(pinfo.size() == numPrimitives);
      
      /* free double buffer presplit items */
      alignedFree(tmp_presplitItem);		
      alignedFree(presplitItem);
      
      return pinfo;	
    }
  }
}