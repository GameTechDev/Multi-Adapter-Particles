//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************
static float softeningSquared = 25;
static float g_fParticleMass = 70000;

//
// Body to body interaction, acceleration of the particle at position 
// bi is updated.
//
void bodyBodyInteraction(inout float3 ai, float4 bj, float4 bi, float mass, int particles) 
{
    float3 r = bj.xyz - bi.xyz;

    float distSqr = dot(r, r);
    distSqr += softeningSquared;

    float invDist = 1.0f / sqrt(distSqr);
    float invDistCube =  invDist * invDist * invDist;
    
    float s = mass * invDistCube * particles;

    ai += r * s;
}

cbuffer cbCS : register(b0)
{
    uint4   g_param;    // param[0] = MAX_PARTICLES;
                        // param[1] = dimx;
    float4  g_paramf;   // paramf[0] = 0.1f;
                        // paramf[1] = 1; 
};

struct Position
{
    float4 pos;
};

struct Velocity
{
    float3 velocity;
};

RWStructuredBuffer<Position> oldPosition    : register(u1);
RWStructuredBuffer<Position> newPosition    : register(u0);

RWStructuredBuffer<Velocity> oldVelocity  : register(u4);
RWStructuredBuffer<Velocity> newVelocity  : register(u3);

// update particle position & velocity
// gravity well located at 0, 0, 0
[numthreads(blocksize, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    float4 pos = oldPosition[DTid.x].pos;
    float3 vel = oldVelocity[DTid.x].velocity;
    float mass = g_fParticleMass;

    float3 r = pos.xyz;

    float distSqr = dot(r, r);
    distSqr += softeningSquared;

    float invDist = -1.0f / sqrt(distSqr);
    float invDistCube = invDist * invDist * invDist;
    float s = mass * invDistCube;

    float3 accel = r * s;

    vel.xyz += accel.xyz * g_paramf.x;        //deltaTime;
    vel.xyz *= g_paramf.y;                    //damping;
    pos.xyz += vel.xyz * g_paramf.x;          //deltaTime;

    newPosition[DTid.x].pos = float4(pos.xyz, length(accel));
    newVelocity[DTid.x].velocity = vel;
}
