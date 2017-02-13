#include "SBVH.h"
#include <algorithm>

// This comparator is used to sort bvh nodes based on its centroid's maximum extent
struct CompareCentroid
{
	CompareCentroid(int dim) : dim(dim) {};

	int dim;
	bool operator()(const SBVHGeometryInfo node1, SBVHGeometryInfo node2) const {
		return node1.bbox.m_centroid[dim] < node2.bbox.m_centroid[dim];
	}
};

void 
SBVH::Build(
	std::vector<std::shared_ptr<Geometry>>& geoms
)
{
	m_geoms = geoms;

	// Initialize geom info
	std::vector<SBVHGeometryInfo> geomInfos(geoms.size());
	for (size_t i = 0; i < geomInfos.size(); i++)
	{
		geomInfos[i] = { i, geoms[i]->GetBBox() };
	}

	size_t totalNodes = 0;

	std::vector<std::shared_ptr<Geometry>> orderedGeoms;
	m_root = BuildRecursive(geomInfos, 0, geomInfos.size(), totalNodes, orderedGeoms);
	m_geoms.swap(orderedGeoms);
	Flatten();
}

void PartitionEqualCounts(int dim, int first, int last, int& mid, std::vector<SBVHGeometryInfo>& geomInfos) {
	// Partial sorting along the maximum extent and split at the middle
	// Sort evenly to each half
	mid = (first + last) / 2;
	std::nth_element(&geomInfos[first], &geomInfos[mid], &geomInfos[last - 1] + 1, CompareCentroid(dim));

}

SBVHNode*
SBVH::BuildRecursive(
	std::vector<SBVHGeometryInfo>& geomInfos, 
	int first, 
	int last, 
	size_t& nodeCount,
	std::vector<std::shared_ptr<Geometry>>& orderedGeoms
	) 
{
	if (last < first || last < 0 || first < 0)
	{
		return nullptr;
	}

	// Compute bounds of all geometries in node
	BBox bboxAllGeoms;
	for (int i = first; i < last; i++) {
		bboxAllGeoms = BBox::BBoxUnion(bboxAllGeoms, geomInfos[i].bbox);
	}

	int numPrimitives = last - first;
	
	if (numPrimitives == 1) {
		// Create a leaf node
		size_t firstGeomOffset = orderedGeoms.size();

		for (int i = first; i < last; i++) {
			int geomIdx = geomInfos[i].geometryId;
			orderedGeoms.push_back(m_geoms[geomIdx]);
		}
		SBVHLeaf* leaf = new SBVHLeaf(nullptr, nodeCount, firstGeomOffset, numPrimitives, bboxAllGeoms);
		nodeCount++;
		return leaf;
	}

	// Choose a dimension to split
	BBox bboxCentroids;
	for (int i = first; i < last; i++) {
		bboxCentroids = BBox::BBoxUnion(bboxCentroids, geomInfos[i].bbox.m_centroid);
	}
	auto dim = static_cast<int>((BBox::BBoxMaximumExtent(bboxCentroids)));
	// If all centroids are the same, create leafe since there's no effective way to split the tree
	if (bboxCentroids.m_max[dim] == bboxCentroids.m_min[dim]) {
		// Create a leaf node
		size_t firstGeomOffset = orderedGeoms.size();

		for (int i = first; i < last; i++)
		{
			int geomIdx = geomInfos[i].geometryId;
			orderedGeoms.push_back(m_geoms[geomIdx]);
		}
		SBVHLeaf* leaf = new SBVHLeaf(nullptr, nodeCount, firstGeomOffset, numPrimitives, bboxAllGeoms);
		nodeCount++;
		return leaf;
	}

	// Create 12 buckets (based from pbrt)
	struct BucketInfo
	{
		int count = 0;
		BBox bbox;
		int enter = 0; // Number of entering references
		int exit = 0; // Number of exiting references 
	};

	const float COST_TRAVERSAL = 0.125f;
	const float COST_INTERSECTION = 1.0f;

	const int NUM_BUCKET = 12;
	BucketInfo buckets[NUM_BUCKET];
	BucketInfo spatialSplitBuckets[NUM_BUCKET];
	float costs[NUM_BUCKET - 1];
	// For quick computation, store the inverse of all geoms surface area
	float invAllGeometriesSA = 1.0f / bboxAllGeoms.GetSurfaceArea();

	int mid;
	switch(m_splitMethod) {
		case SpatialSplit_SAH:
			// 1. Find object split candidate
			// Restrict spatial split of crossed threshold
			// 2. Find spatial split candidate
			// 3. Select winner candidate
			if (numPrimitives <= 4)
			{
				PartitionEqualCounts(dim, first, last, mid, geomInfos);
			}
			else
			{
				const float RESTRICT_ALPHA = 0.2f;

				// === FIND OBJECT SPLIT CANDIDATE
				// For each primitive in range, determine which bucket it falls into
				for (int i = first; i < last; i++)
				{
					int whichBucket = NUM_BUCKET * bboxCentroids.Offset(geomInfos.at(i).bbox.m_centroid)[dim];
					assert(whichBucket <= NUM_BUCKET);
					if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;

					buckets[whichBucket].count++;
					buckets[whichBucket].bbox = BBox::BBoxUnion(buckets[whichBucket].bbox, geomInfos.at(i).bbox);
				}

				// somehow we need to figure out how to clip primitives against bbox
				// For each primitive in range, determine which bucket it falls into
				// If a primitive straddles across multiple buckets, chop it into
				// multiple tight bounding boxes that only fits inside each bucket
				float bucketSize = (bboxAllGeoms.m_max[dim] - bboxAllGeoms.m_min[dim]) / NUM_BUCKET;
				for (int i = first; i < last; i++)
				{
					// Check if geometry straddles across buckets
					int startEdgeBucket = NUM_BUCKET * bboxAllGeoms.Offset(geomInfos.at(i).bbox.m_min)[dim];;
					assert(startEdgeBucket <= NUM_BUCKET);
					if (startEdgeBucket == NUM_BUCKET) startEdgeBucket = NUM_BUCKET - 1;
					int endEdgeBucket = NUM_BUCKET * bboxAllGeoms.Offset(geomInfos.at(i).bbox.m_max)[dim];;
					assert(endEdgeBucket <= NUM_BUCKET);
					if (endEdgeBucket == NUM_BUCKET) endEdgeBucket = NUM_BUCKET - 1;

					if (startEdgeBucket != endEdgeBucket) {
						// Geometry straddles across multiple buckets, split it
						geomInfos[i].straddling = true;

						vec3 pointOnSplittingPlane = bboxAllGeoms.m_min;
						for (auto bucket = startEdgeBucket; bucket < endEdgeBucket; bucket++) {
							// Get the primitive reference from geometry info, then clip it against the splitting plane
							std::shared_ptr<Geometry> pGeom = m_geoms[geomInfos[i].geometryId];

							// Cast to triangle
							Triangle* pTri = dynamic_cast<Triangle*>(pGeom.get());
							if (pTri == nullptr) {
								// not a triangle
								// Geometry reference completely falls inside bucket
								int whichBucket = NUM_BUCKET * bboxCentroids.Offset(geomInfos.at(i).bbox.m_centroid)[dim];
								assert(whichBucket <= NUM_BUCKET);
								if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;

								spatialSplitBuckets[whichBucket].count++;
								spatialSplitBuckets[whichBucket].bbox = BBox::BBoxUnion(buckets[whichBucket].bbox, geomInfos.at(i).bbox);
								geomInfos[i].straddling = false;
								break;

							}

							// Find intersection with splitting plane
							pointOnSplittingPlane[dim] = bucketSize * (bucket + 1) + bboxAllGeoms.m_min[dim];

							vec3 planeNormal;
							EAxis splittinPlaneAxis[2];
							switch(dim) {
								case EAxis::X:
									// Find Y and Z intersections
									planeNormal = vec3(1, 0, 0);
									splittinPlaneAxis[0] = EAxis::Y;
									splittinPlaneAxis[1] = EAxis::Z;
									break;
								case EAxis::Y:
									// Find X and Z intersections
									planeNormal = vec3(0, 1, 0);
									splittinPlaneAxis[0] = EAxis::X;
									splittinPlaneAxis[1] = EAxis::Z;
									break;
								case EAxis::Z:
									// Find Y and X intersections
									planeNormal = vec3(0, 0, 1);
									splittinPlaneAxis[0] = EAxis::X;
									splittinPlaneAxis[1] = EAxis::Y;
									break;
							}

							bool isVert0BelowPlane = dot(pTri->vert0 - pointOnSplittingPlane, planeNormal) < 0;
							bool isVert1BelowPlane = dot(pTri->vert1 - pointOnSplittingPlane, planeNormal) < 0;
							bool isVert2BelowPlane = dot(pTri->vert2 - pointOnSplittingPlane, planeNormal) < 0;

							// Assert that not all vertices lie on the same side of the splitting plane
							assert(!(isVert0BelowPlane && isVert1BelowPlane && isVert2BelowPlane));
							assert(!(!isVert0BelowPlane && !isVert1BelowPlane && !isVert2BelowPlane));

							// Use similar triangles to find the point of intersection on splitting plane
							vec3 pointAbove;
							vec3 pointsBelow[2];
							if (isVert0BelowPlane && isVert1BelowPlane)
							{
								pointAbove = pTri->vert2;
								pointsBelow[0] = pTri->vert0;
								pointsBelow[1] = pTri->vert1;
							}
							else if (isVert0BelowPlane && isVert2BelowPlane)
							{
								pointAbove = pTri->vert1;
								pointsBelow[0] = pTri->vert0;
								pointsBelow[1] = pTri->vert2;
							}
							else if (isVert1BelowPlane && isVert2BelowPlane)
							{
								pointAbove = pTri->vert0;
								pointsBelow[0] = pTri->vert1;
								pointsBelow[1] = pTri->vert2;
							}

							std::vector<vec3> isxPoints;
							for (auto pointBelow : pointsBelow) {
								vec3 isxPoint;
								for (auto axis = 0; axis < 2; axis++)
								{
									isxPoint[dim] = pointOnSplittingPlane[dim];
									isxPoint[splittinPlaneAxis[axis]] = 
										((pointBelow[splittinPlaneAxis[axis]] - pointAbove[splittinPlaneAxis[axis]]) /
										(pointBelow[dim] - pointAbove[dim])) *
										(pointOnSplittingPlane[dim] - pointAbove[dim]) + 
										pointAbove[splittinPlaneAxis[axis]];
								}
								isxPoints.push_back(isxPoint);
							}

							// Found intersection, grow tight bounding box of reference for bucket
							SBVHGeometryInfo tightBBoxGeomInfo;

							// Create additional geometry info to whole the new tight bbox
							tightBBoxGeomInfo.geometryId = geomInfos[i].geometryId;
							tightBBoxGeomInfo.bbox = BBox::BBoxFromPoints(isxPoints);
							if (pTri->vert0[dim] > pointOnSplittingPlane[dim] - bucketSize && 
								pTri->vert0[dim] < pointOnSplittingPlane[dim]) {
								tightBBoxGeomInfo.bbox = BBox::BBoxUnion(tightBBoxGeomInfo.bbox, pTri->vert0);
							}
							if (pTri->vert1[dim] > pointOnSplittingPlane[dim] - bucketSize &&
								pTri->vert1[dim] < pointOnSplittingPlane[dim])
							{
								tightBBoxGeomInfo.bbox = BBox::BBoxUnion(tightBBoxGeomInfo.bbox, pTri->vert1);
							}
							if (pTri->vert2[dim] > pointOnSplittingPlane[dim] - bucketSize &&
								pTri->vert2[dim] < pointOnSplittingPlane[dim])
							{
								tightBBoxGeomInfo.bbox = BBox::BBoxUnion(tightBBoxGeomInfo.bbox, pTri->vert2);
							}

							geomInfos.push_back(tightBBoxGeomInfo);

							spatialSplitBuckets[bucket].bbox = BBox::BBoxUnion(tightBBoxGeomInfo.bbox, spatialSplitBuckets[bucket].bbox);
						}

						// Increment entering and exiting reference counts
						spatialSplitBuckets[startEdgeBucket].enter++;
						spatialSplitBuckets[endEdgeBucket].exit++;
					} else {
						// Geometry reference completely falls inside bucket
						int whichBucket = NUM_BUCKET * bboxCentroids.Offset(geomInfos.at(i).bbox.m_centroid)[dim];
						assert(whichBucket <= NUM_BUCKET);
						if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;

						spatialSplitBuckets[whichBucket].count++;
						spatialSplitBuckets[whichBucket].bbox = BBox::BBoxUnion(buckets[whichBucket].bbox, geomInfos.at(i).bbox);

					}
				}

				// Compute cost for splitting after each bucket
				float minSplitCost = INFINITY;
				float minSplitCostBucket = 0;
				float objectSplitCost = INFINITY;
				float spatialSplitCost = INFINITY;
				bool isSpatialSplit = false;
				for (int i = 0; i < NUM_BUCKET - 1; i++)
				{
					BBox bbox0, bbox1;
					int count0 = 0, count1 = 0;

					// Compute cost for buckets before split candidate
					for (int j = 0; j <= i; j++)
					{
						bbox0 = BBox::BBoxUnion(bbox0, buckets[j].bbox);
						count0 += buckets[j].count;
					}

					// Compute cost for buckets after split candidate
					for (int j = i + 1; j < NUM_BUCKET; j++)
					{
						bbox1 = BBox::BBoxUnion(bbox1, buckets[j].bbox);
						count1 += buckets[j].count;
					}

					objectSplitCost = COST_TRAVERSAL + COST_INTERSECTION * (count0 * bbox0.GetSurfaceArea() + count1 * bbox1.GetSurfaceArea()) * invAllGeometriesSA;

					// Store children overlapping SA so we can use to restrict spatial split later
					float SAChildrenOverlap = BBox::BBoxOverlap(bbox0, bbox1).GetSurfaceArea();
					// === RESTRICT SPATIAL SPLIT
					// If the cost of B1 and B2 overlap is cheap enough, we can skip spatial splitting
					// @todo: Maybe this value can be controlled with a GUI?
					if (SAChildrenOverlap * invAllGeometriesSA > RESTRICT_ALPHA)
					{
						// Object split overlap is too expensive, consider spatial split
						// === FIND SPATIAL SPLIT CANDIDATE

						bbox0 = BBox();
						bbox1 = BBox();

						// Compute cost for buckets before split candidate
						for (int j = 0; j <= i; j++)
						{
							bbox0 = BBox::BBoxUnion(bbox0, spatialSplitBuckets[j].bbox);
							count0 += spatialSplitBuckets[j].count;
						}

						// Compute cost for buckets after split candidate
						for (int j = i + 1; j < NUM_BUCKET; j++)
						{
							bbox1 = BBox::BBoxUnion(bbox1, spatialSplitBuckets[j].bbox);
							count1 += spatialSplitBuckets[j].count;
						}

						spatialSplitCost = COST_TRAVERSAL + COST_INTERSECTION * (count0 * bbox0.GetSurfaceArea() + count1 * bbox1.GetSurfaceArea()) * invAllGeometriesSA;
					}

					// Get the cheapest cost between object split candidate and spatial split candidate
					
					if (objectSplitCost < spatialSplitCost)
					{
						if (objectSplitCost < minSplitCost) {
							minSplitCost = objectSplitCost;
							minSplitCostBucket = i;
							isSpatialSplit = false;
						}
					} else {
						if (spatialSplitCost < minSplitCost) {
							minSplitCost = spatialSplitCost;
							minSplitCostBucket = i;
							isSpatialSplit = true;
						}
					}
				}

				// Do a pass to remove straddling geominfos
				if (isSpatialSplit) {
					std::remove_if(&geomInfos[first], &geomInfos[last], [=](SBVHGeometryInfo& gi)
					{
						return gi.straddling == true;
					});
				}

				// == CREATE LEAF OF SPLIT
				float leafCost = numPrimitives * COST_INTERSECTION;
				if (numPrimitives > m_maxGeomsInNode || minSplitCost < leafCost)
				{
					// Split node

					if (isSpatialSplit) {
						SBVHGeometryInfo *pmid = std::partition(
							&geomInfos[first], &geomInfos[last - 1] + 1,
							[=](const SBVHGeometryInfo& gi)
						{
							// Partition geometry into two halves, before and after the split
							int whichBucket = NUM_BUCKET * bboxCentroids.Offset(gi.bbox.m_centroid)[dim];
							assert(whichBucket <= NUM_BUCKET);
							if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;
							return whichBucket <= minSplitCostBucket;
						});
						mid = pmid - &geomInfos[0];

					} else {
						SBVHGeometryInfo *pmid = std::partition(
							&geomInfos[first], &geomInfos[last - 1] + 1,
							[=](const SBVHGeometryInfo& gi)
						{
							// Partition geometry into two halves, before and after the split
							int whichBucket = NUM_BUCKET * bboxCentroids.Offset(gi.bbox.m_centroid)[dim];
							assert(whichBucket <= NUM_BUCKET);
							if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;
							return whichBucket <= minSplitCostBucket;
						});
						mid = pmid - &geomInfos[0];
					}
				}
				else
				{

					// Cost of splitting buckets is too high, create a leaf node instead
					size_t firstGeomOffset = orderedGeoms.size();

					for (int i = first; i < last; i++)
					{
						int geomIdx = geomInfos[i].geometryId;
						orderedGeoms.push_back(m_geoms[geomIdx]);
					}
					SBVHLeaf* leaf = new SBVHLeaf(nullptr, nodeCount, firstGeomOffset, numPrimitives, bboxAllGeoms);
					nodeCount++;
					return leaf;
				}
			}
			break;

		case SAH: // Partition based on surface area heuristics
			if (numPrimitives <= 4)
			{
				PartitionEqualCounts(dim, first, last, mid, geomInfos);
			} 
			else 
			{
				// For each primitive in range, determine which bucket it falls into
				for (int i = first; i < last; i++) {
					int whichBucket = NUM_BUCKET * bboxCentroids.Offset(geomInfos.at(i).bbox.m_centroid)[dim];
					assert(whichBucket <= NUM_BUCKET);
					if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;
					
					buckets[whichBucket].count++;
					buckets[whichBucket].bbox = BBox::BBoxUnion(buckets[whichBucket].bbox, geomInfos.at(i).bbox);
				}

				// Compute cost for splitting after each bucket
				for (int i = 0; i < NUM_BUCKET - 1; i++) {
					BBox bbox0, bbox1;
					int count0 = 0, count1 = 0;

					// Compute cost for buckets before split candidate
					for (int j = 0; j <= i ; j++) {
						bbox0 = BBox::BBoxUnion(bbox0, buckets[j].bbox);
						count0 += buckets[j].count;
					}

					// Compute cost for buckets after split candidate
					for (int j = i  + 1; j < NUM_BUCKET; j++)
					{
						bbox1 = BBox::BBoxUnion(bbox1, buckets[j].bbox);
						count1 += buckets[j].count;
					}

					costs[i] = COST_TRAVERSAL + COST_INTERSECTION * (count0 * bbox0.GetSurfaceArea() + count1 * bbox1.GetSurfaceArea()) * invAllGeometriesSA;
				}

				// Now that we have the costs, we can loop through our buckets and find
				// which bucket has the lowest cost
				float minCost = costs[0];
				int minCostBucket = 0;
				for (int i = 1; i < NUM_BUCKET - 1; i++) {
					if (costs[i] < minCost) {
						minCost = costs[i];
						minCostBucket = i;
					}
				}

				// Either create a leaf or split
				float leafCost = numPrimitives;
				if (numPrimitives > m_maxGeomsInNode || minCost < leafCost) {
					// Split node
					SBVHGeometryInfo *pmid = std::partition(
						&geomInfos[first], &geomInfos[last - 1] + 1,
						[=](const SBVHGeometryInfo& gi)
					{
						// Partition geometry into two halves, before and after the split
						int whichBucket = NUM_BUCKET * bboxCentroids.Offset(gi.bbox.m_centroid)[dim];
						assert(whichBucket <= NUM_BUCKET);
						if (whichBucket == NUM_BUCKET) whichBucket = NUM_BUCKET - 1;
						return whichBucket <= minCostBucket;
					});
					mid = pmid - &geomInfos[0];

				} 
				else {

					// Cost of splitting buckets is too high, create a leaf node instead
					size_t firstGeomOffset = orderedGeoms.size();

					for (int i = first; i < last; i++)
					{
						int geomIdx = geomInfos[i].geometryId;
						orderedGeoms.push_back(m_geoms[geomIdx]);
					}
					SBVHLeaf* leaf = new SBVHLeaf(nullptr, nodeCount, firstGeomOffset, numPrimitives, bboxAllGeoms);
					nodeCount++;
					return leaf;
				}
			}
			break;

		case EqualCounts: // Partition based on equal counts
		default:
			PartitionEqualCounts(dim, first, last, mid, geomInfos);
			break;
	}

	// Build near child
	EAxis splitAxis = static_cast<EAxis>(dim);;
	SBVHNode* nearChild = BuildRecursive(geomInfos, first, mid, nodeCount, orderedGeoms);

	// Build far child
	SBVHNode* farChild = BuildRecursive(geomInfos, mid, last, nodeCount, orderedGeoms);

	SBVHNode* node = new SBVHNode(nearChild, farChild, nodeCount, splitAxis);
	nodeCount++;
	return node;
}


Intersection SBVH::GetIntersection(const Ray& r) 
{
	float nearestT = INFINITY;
	Intersection nearestIsx; 

	if (m_root->IsLeaf()) {
		SBVHLeaf* leaf = dynamic_cast<SBVHLeaf*>(m_root);
		for (int i = 0; i < leaf->m_numGeoms; i++)
		{
			std::shared_ptr<Geometry> geom = m_geoms[leaf->m_firstGeomOffset + i];
			Intersection isx = geom->GetIntersection(r);
			if (isx.t > 0 && isx.t < nearestT)
			{
				nearestT = isx.t;
				nearestIsx = isx;
			}
		}
		return nearestIsx;
	}

	// Traverse children
	GetIntersectionRecursive(r, m_root->m_nearChild, nearestT, nearestIsx);
	GetIntersectionRecursive(r, m_root->m_farChild, nearestT, nearestIsx);

	return nearestIsx;
}


void SBVH::GetIntersectionRecursive(
	const Ray& r, 
	SBVHNode* node, 
	float& nearestT, 
	Intersection& nearestIsx
	) 
{
	if (node == nullptr) {
		return;
	}

	if (node->IsLeaf()) {
		SBVHLeaf* leaf = dynamic_cast<SBVHLeaf*>(node);

		// Return nearest primitive
		for (int i = 0; i < leaf->m_numGeoms; i++)
		{
			std::shared_ptr<Geometry> geom = m_geoms[leaf->m_firstGeomOffset + i];
			Intersection isx = geom->GetIntersection(r);
			if (isx.t > 0 && isx.t < nearestT)
			{
				nearestT = isx.t;
				nearestIsx = isx;
			}
		}
		return;
	}

	if (node->m_bbox.DoesIntersect(r))
	{
		// Traverse children
		GetIntersectionRecursive(r, node->m_nearChild, nearestT, nearestIsx);
		GetIntersectionRecursive(r, node->m_farChild, nearestT, nearestIsx);
	}
}


bool SBVH::DoesIntersect(
	const Ray& r
	)
{
	if (m_root->IsLeaf())
	{
		SBVHLeaf* leaf = dynamic_cast<SBVHLeaf*>(m_root);

		for (int i = 0; i < leaf->m_numGeoms; i++)
		{
			std::shared_ptr<Geometry> geom = m_geoms[leaf->m_firstGeomOffset + i];
			Intersection isx = geom->GetIntersection(r);
			if (isx.t > 0)
			{
				return true;
			}
		}
		return false;
	}

	// Traverse children
	return DoesIntersectRecursive(r, m_root->m_nearChild) || DoesIntersectRecursive(r, m_root->m_farChild);
}

bool SBVH::DoesIntersectRecursive(
	const Ray& r, 
	SBVHNode* node
	)
{
	if (node == nullptr)
	{
		return false;
	}

	if (node->IsLeaf())
	{
		SBVHLeaf* leaf = dynamic_cast<SBVHLeaf*>(node);
		for (int i = 0; i < leaf->m_numGeoms; i++)
		{
			std::shared_ptr<Geometry> geom = m_geoms[leaf->m_firstGeomOffset + i];
			Intersection isx = geom->GetIntersection(r);
			if (isx.t > 0)
			{
				return true;
			}
		}

		return false;
	}

	if (node->m_bbox.DoesIntersect(r))
	{
		// Traverse children
		return DoesIntersectRecursive(r, node->m_nearChild) || DoesIntersectRecursive(r, node->m_farChild);
	}
	return false;
}



void SBVH::Destroy() {
	DestroyRecursive(m_root);
}

void 
SBVH::DestroyRecursive(SBVHNode* node) {

	if (node == nullptr) {
		return;
	}

	if (node->IsLeaf())
	{
		delete node;
		node = nullptr;
		return;
	}

	DestroyRecursive(node->m_nearChild);
	DestroyRecursive(node->m_farChild);

	delete node;
	node == nullptr;
}

void SBVH::FlattenRecursive(
	SBVHNode* node
	)
{
	if (node == nullptr)
	{
		return;
	}

	m_nodes.push_back(node);
	FlattenRecursive(node->m_nearChild);
	FlattenRecursive(node->m_farChild);
}

void SBVH::Flatten() {

	FlattenRecursive(m_root);
}

