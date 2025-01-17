#pragma once

#include <glm/glm.hpp>
#include "sceneStructs.h"
#include "utilities.h"
#include "thrust/sort.h"

__device__ AABB computeBBox(Triangle tri)
{
	AABB bbox;
	for (int i = 0; i < 3; i++)
	{
		bbox.max[i] = max(max(tri.v1[i], tri.v2[i]), tri.v3[i]);
		bbox.min[i] = min(min(tri.v1[i], tri.v2[i]), tri.v3[i]);
	}
	return bbox;
}

__device__ unsigned int expandBits(unsigned int v)
{
	v = (v * 0x00010001u) & 0xFF0000FFu;
	v = (v * 0x00000101u) & 0x0F00F00Fu;
	v = (v * 0x00000011u) & 0xC30C30C3u;
	v = (v * 0x00000005u) & 0x49249249u;
	return v;
}

//input the aabb box of a triangle
//generate a 30-bit morton code
__device__ unsigned int genMortonCode(AABB bbox, glm::vec3 geoMin, glm::vec3 geoMax)
{
	float x = (bbox.min.x + bbox.max.x) * 0.5f;
	float y = (bbox.min.y + bbox.max.y) * 0.5f;
	float z = (bbox.min.y + bbox.max.y) * 0.5f;
	float normalizedX = (x - geoMin.x) / (geoMax.x - geoMin.x);
	float normalizedY = (y - geoMin.y) / (geoMax.y - geoMin.y);
	float normalizedZ = (z - geoMin.z) / (geoMax.z - geoMin.z);

	normalizedX = min(max(normalizedX * 1024.0f, 0.0f), 1023.0f);
	normalizedY = min(max(normalizedY * 1024.0f, 0.0f), 1023.0f);
	normalizedZ = min(max(normalizedZ * 1024.0f, 0.0f), 1023.0f);

	unsigned int xx = expandBits((unsigned int)normalizedX);
	unsigned int yy = expandBits((unsigned int)normalizedY);
	unsigned int zz = expandBits((unsigned int)normalizedZ);

	return xx * 4 + yy * 2 + zz;
}


__device__ unsigned long long expandMorton(int index,unsigned int mortonCode)
{
	unsigned long long exMortonCode = mortonCode;
	exMortonCode <<= 32;
	exMortonCode += index;
	return exMortonCode;
}

/**
* please sort the morton code first then get split pairs
thrust::stable_sort_by_key(mortonCodes, mortonCodes + triangleCount, triangleIndex);*/

//total input is a 30 x N matrix
//currentIndex is between 0 - N-1
//the input morton codes should be in the reduced form, no same elements are expected to appear twice!
__device__ int getSplit(unsigned int* mortonCodes, unsigned int currIndex, int nextIndex, unsigned int bound)
{
	if (nextIndex < 0 || nextIndex >= bound)
		return -1;
	//NOTE: if use small size model, this step can be skipped
	// just to ensure the morton codes are unique!
	//unsigned int mask = mortonCodes[currIndex] ^ mortonCodes[nextIndex];
	unsigned long long mask = expandMorton(currIndex, mortonCodes[currIndex]) ^ expandMorton(nextIndex, mortonCodes[nextIndex]);
	// __clzll gives the number of consecutive zero bits in that number
	// this gives us the index of the most significant bit between the two numbers
	int commonPrefix = __clzll(mask);
	return commonPrefix;
}

__device__ int getSign(int tmp)
{
	if (tmp > 0)
		return 1;
	else
		return -1;
	//return (tmp > 0) - (tmp < 0);
}

__device__ void buildBBox(BVHNode& curr, BVHNode left, BVHNode right)
{
	glm::vec3 newMin;
	glm::vec3 newMax;
	newMin.x = min(left.bbox.min.x, right.bbox.min.x);
	newMax.x = max(left.bbox.max.x, right.bbox.max.x);
	newMin.y = min(left.bbox.min.y, right.bbox.min.y);
	newMax.y = max(left.bbox.max.y, right.bbox.max.y);
	newMin.z = min(left.bbox.min.z, right.bbox.min.z);
	newMax.z = max(left.bbox.max.z, right.bbox.max.z);

	curr.bbox = AABB{newMin, newMax};
	curr.isLeaf = 0;
}

// build the bounding box and morton code for each triangle
__global__ void buildLeafMorton(int startIndex, int numTri, float minX, float minY, float minZ, 
	float maxX, float maxY, float maxZ, Triangle* triangles, BVHNode* leafNodes,
	unsigned int* mortonCodes)
{
	int ind = blockIdx.x * blockDim.x + threadIdx.x;
	if (ind < numTri)
	{
		int leafPos = ind + numTri - 1;
		Triangle tri = triangles[ind + startIndex];
		leafNodes[leafPos].bbox = computeBBox(tri);
		leafNodes[leafPos].isLeaf = 1;
		leafNodes[leafPos].leftIndex = -1;
		leafNodes[leafPos].rightIndex = -1;
		leafNodes[leafPos].triangleIndex = ind;
		mortonCodes[ind] = genMortonCode(leafNodes[ind + numTri - 1].bbox, glm::vec3(minX, minY, minZ), glm::vec3(maxX, maxY, maxZ));
	}
}


//input the unique morton code
//codeCount is the size of the unique morton code
//splitList is 30 x N list
// the size of unique morton is less than 2^30 : [1, 2^30]
__global__ void buildSplitList(int codeCount, unsigned int* uniqueMorton, BVHNode* nodes)
{
	int ind = blockIdx.x * blockDim.x + threadIdx.x;
	if (ind < codeCount - 1)
	{
		int sign = getSign(getSplit(uniqueMorton, ind, ind + 1, codeCount) - getSplit(uniqueMorton, ind, ind - 1, codeCount));
		int dMin = getSplit(uniqueMorton, ind, ind - sign, codeCount);
		int lenMax = 2;
		int k = getSplit(uniqueMorton, ind, ind + lenMax * sign, codeCount);
		while (k > dMin)
		{
			lenMax *= 2;
			k = getSplit(uniqueMorton, ind, ind + lenMax * sign, codeCount);
		}

		int len = 0;
		int last = lenMax >> 1;
		while (last > 0)
		{
			int tmp = ind + (len + last) * sign;
			int diff = getSplit(uniqueMorton, ind, tmp, codeCount);
			if (diff > dMin)
			{
				len = len + last;
			}
			last >>= 1;
		}
		//last in range
		int j = ind + len * sign;

		int currRange = getSplit(uniqueMorton, ind, j, codeCount);
		int split = 0;
		do {
			len = (len + 1) >> 1;
			if (getSplit(uniqueMorton, ind, ind + (split + len) * sign, codeCount) > currRange)
			{
				split += len;
			}
		} while (len > 1);

		int tmp = ind + split * sign + min(sign, 0);

		if (min(ind, j) == tmp)
		{
			//leaf node
			// the number of internal nodes is N - 1
			nodes[ind].leftIndex = tmp + codeCount - 1;
			nodes[tmp + codeCount - 1].parent = ind;
		}
		else
		{
			// internal node
			nodes[ind].leftIndex = tmp;
			nodes[tmp].parent = ind;
		}
		if (max(ind, j) == tmp + 1)
		{
			nodes[ind].rightIndex = tmp + codeCount;
			nodes[tmp + codeCount].parent = ind;
		}
		else
		{
			nodes[ind].rightIndex = tmp + 1;
			nodes[tmp + 1].parent = ind;
		}
	}
	
}


// very naive implementation
__global__ void buildBBoxes(int leafCount, BVHNode* nodes, unsigned char* ready)
{
	int ind = blockIdx.x * blockDim.x + threadIdx.x;
	// only update internal node
	if (ind < leafCount - 1)
	{
		BVHNode node = nodes[ind];
		if (ready[ind] != 0)
			return;
		if(ready[node.leftIndex] != 0 && ready[node.rightIndex] != 0)
		{
			buildBBox(nodes[ind], nodes[node.leftIndex], nodes[node.rightIndex]);
			ready[ind] = 1;
		}
	}
}


