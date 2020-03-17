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
#pragma once

// project defines
// enable/disable is always 1/0

#define X_STRINGIFY(x) #x
#define STRINGIFY( x ) X_STRINGIFY(x)

#define MINIMUM_D3D_FEATURE_LEVEL D3D_FEATURE_LEVEL_12_0

// compute shader block size
#define BLOCK_SIZE 64
// UI or no?
#define IMGUI_ENABLED 1

#define INITIAL_PARTICLE_SPEED 15.0f
#define INITIAL_PARTICLE_SIZE 2.5f
#define INITIAL_PARTICLE_INTENSITY 0.15f
#define PARTICLE_SPREAD 400.0f

#define MIN_NUM_PARTICLES 256*1024
