/*
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "actionNet.h"
#include "imageNet.h"

#include "tensorConvert.h"

#include "cudaMappedMemory.h"
#include "cudaResize.h"

#include "commandLine.h"
#include "filesystem.h"
#include "logging.h"


// constructor
actionNet::actionNet() : tensorNet()
{
	mNetworkType  = CUSTOM;
	mNumClasses   = 0;
	mNumFrames    = 0;
	
	mInputBuffers[0] = NULL;
	mInputBuffers[1] = NULL;
	
	mCurrentInputBuffer = 0;
	mCurrentFrameIndex  = 0;
}


// destructor
actionNet::~actionNet()
{
	if( mInputBuffers[0] != NULL )
		mInputs[0].CUDA = mInputBuffers[0];  // restore this pointer so it's properly deleted in tensorNet destructor
	
	CUDA_FREE(mInputBuffers[1]);
}


// Create
actionNet* actionNet::Create( actionNet::NetworkType networkType, uint32_t maxBatchSize, 
					   precisionType precision, deviceType device, bool allowGPUFallback )
{
	actionNet* net = NULL;
	
	if( networkType == RESNET_18 )
		net = Create("networks/Action-ResNet18/resnet-18-kinetics-moments.onnx", "networks/Action-ResNet18/labels.txt", "0", "198", maxBatchSize, precision, device, allowGPUFallback);

	if( !net )
	{
		LogError(LOG_TRT "actionNet -- invalid built-in model '%s' requested\n", actionNet::NetworkTypeToStr(networkType));
		return NULL;
	}
	
	net->mNetworkType = networkType;
	return net;
}



// Create
actionNet* actionNet::Create( int argc, char** argv )
{
	return Create(commandLine(argc, argv));
}


// Create
actionNet* actionNet::Create( const commandLine& cmdLine )
{
	actionNet* net = NULL;

	// obtain the network name
	const char* modelName = cmdLine.GetString("network");
	
	if( !modelName )
		modelName = cmdLine.GetString("model", "resnet-18");
	
	// parse the network type
	const actionNet::NetworkType type = NetworkTypeFromStr(modelName);

	if( type == actionNet::CUSTOM )
	{
		const char* labels = cmdLine.GetString("labels");
		const char* input  = cmdLine.GetString("input_blob");
		const char* output = cmdLine.GetString("output_blob");

		if( !input ) 	input    = ACTIONNET_DEFAULT_INPUT;
		if( !output )  output   = ACTIONNET_DEFAULT_OUTPUT;
		
		int maxBatchSize = cmdLine.GetInt("batch_size");
		
		if( maxBatchSize < 1 )
			maxBatchSize = DEFAULT_MAX_BATCH_SIZE;

		net = actionNet::Create(modelName, labels, input, output, maxBatchSize);
	}
	else
	{
		// create from pretrained model
		net = actionNet::Create(type);
	}

	if( !net )
		return NULL;

	// enable layer profiling if desired
	if( cmdLine.GetFlag("profile") )
		net->EnableLayerProfiler();

	return net;
}


// Create
actionNet* actionNet::Create( const char* model_path, const char* class_path, 
						const char* input, const char* output, 
						uint32_t maxBatchSize, precisionType precision, 
						deviceType device, bool allowGPUFallback )
{
	actionNet* net = new actionNet();
	
	if( !net )
		return NULL;
	
	if( !net->init(model_path, class_path, input, output, maxBatchSize, precision, device, allowGPUFallback) )
	{
		LogError(LOG_TRT "actionNet -- failed to initialize.\n");
		return NULL;
	}
	
	return net;
}



// init
bool actionNet::init(const char* model_path, const char* class_path, 
				 const char* input, const char* output, 
				 uint32_t maxBatchSize, precisionType precision, 
				 deviceType device, bool allowGPUFallback )
{
	if( !model_path || !class_path || !input || !output )
		return false;

	LogInfo("\n");
	LogInfo("actionNet -- loading action recognition network model from:\n");
	LogInfo("          -- model        %s\n", model_path);
	LogInfo("          -- class_labels %s\n", class_path);
	LogInfo("          -- input_blob   '%s'\n", input);
	LogInfo("          -- output_blob  '%s'\n", output);
	LogInfo("          -- batch_size   %u\n\n", maxBatchSize);

	if( !tensorNet::LoadNetwork(NULL, model_path, NULL, input, output, 
						   maxBatchSize, precision, device, allowGPUFallback ) )
	{
		LogError(LOG_TRT "failed to load %s\n", model_path);
		return false;
	}
	
	// setup input ring buffer
	mInputBuffers[0] = mInputs[0].CUDA;
	
	if( CUDA_FAILED(cudaMalloc((void**)&mInputBuffers[1], mInputs[0].size)) )
		return false;
	
	CUDA(cudaMemset(mInputBuffers[1], 0, mInputs[0].size));

	// load classnames
	mNumFrames = mInputs[0].dims.d[1];
	mNumClasses = mOutputs[0].dims.d[0];

	if( !imageNet::LoadClassInfo(class_path, mClassDesc, mNumClasses) || mClassDesc.size() != mNumClasses )
	{
		LogError(LOG_TRT "actionNet -- failed to load class descriptions  (%zu of %u)\n", mClassDesc.size(), mNumClasses);
		return false;
	}
	
	LogSuccess(LOG_TRT "actionNet -- %s initialized.\n", model_path);
	return true;
}
			

// NetworkTypeFromStr
actionNet::NetworkType actionNet::NetworkTypeFromStr( const char* modelName )
{
	if( !modelName )
		return actionNet::CUSTOM;

	actionNet::NetworkType type = actionNet::CUSTOM;

	if( strcasecmp(modelName, "resnet-18") == 0 || strcasecmp(modelName, "resnet_18") == 0 || strcasecmp(modelName, "resnet18") == 0 )
		type = actionNet::RESNET_18;
	else if( strcasecmp(modelName, "resnet-34") == 0 || strcasecmp(modelName, "resnet_34") == 0 || strcasecmp(modelName, "resnet34") == 0 )
		type = actionNet::RESNET_50;
	else if( strcasecmp(modelName, "resnet-50") == 0 || strcasecmp(modelName, "resnet_50") == 0 || strcasecmp(modelName, "resnet50") == 0 )
		type = actionNet::RESNET_50;
	else
		type = actionNet::CUSTOM;

	return type;
}


// NetworkTypeToStr
const char* actionNet::NetworkTypeToStr( actionNet::NetworkType network )
{
	switch(network)
	{
		case actionNet::RESNET_18:	return "ResNet-18";
		case actionNet::RESNET_34:	return "ResNet-34";
		case actionNet::RESNET_50:	return "ResNet-50";
	}

	return "Custom";
}


// PreProcess
bool actionNet::PreProcess( void* image, uint32_t width, uint32_t height, imageFormat format )
{
	// verify parameters
	if( !image || width == 0 || height == 0 )
	{
		LogError(LOG_TRT "actionNet::PreProcess( 0x%p, %u, %u ) -> invalid parameters\n", image, width, height);
		return false;
	}

	if( !imageFormatIsRGB(format) )
	{
		LogError(LOG_TRT "actionNet::Classify() -- unsupported image format (%s)\n", imageFormatToStr(format));
		LogError(LOG_TRT "                        supported formats are:\n");
		LogError(LOG_TRT "                           * rgb8\n");		
		LogError(LOG_TRT "                           * rgba8\n");		
		LogError(LOG_TRT "                           * rgb32f\n");		
		LogError(LOG_TRT "                           * rgba32f\n");

		return false;
	}

	PROFILER_BEGIN(PROFILER_PREPROCESS);

	// input tensor dims are:  3x16x112x112 (CxNxHxW)
	const size_t inputFrameSize = mInputs[0].dims.d[2] * mInputs[0].dims.d[3];
	const size_t inputBatchSize = mInputs[0].dims.d[1] * inputFrameSize;
	
	const uint32_t previousInputBuffer = (mCurrentInputBuffer + 1) % 2;

	// shift the inputs down by one frame
	if( CUDA_FAILED(cudaMemcpy(mInputBuffers[mCurrentInputBuffer],
						  mInputBuffers[previousInputBuffer] + inputFrameSize,
						  mInputs[0].size - (inputFrameSize * sizeof(float)),
						  cudaMemcpyDeviceToDevice)) )
	{
		return false;
	}

	// convert input image into tensor format
	if( IsModelType(MODEL_ONNX) )
	{
		// downsample, convert to band-sequential RGB, and apply pixel normalization, and mean pixel subtraction
		// also apply striding so that the color channels are interleaved across the N frames (CxNxHxW)
		if( CUDA_FAILED(cudaTensorNormMeanRGB(image, format, width, height, 
									   mInputBuffers[mCurrentInputBuffer] + inputFrameSize * mCurrentFrameIndex, 
									   mInputs[0].dims.d[2], mInputs[0].dims.d[3], 
									   make_float2(0.0f, 1.0f), 
									   make_float3(0.4344705882352941f, 0.4050980392156863f, 0.3774901960784314f),
									   make_float3(1.0f, 1.0f, 1.0f), 
									   GetStream(), inputBatchSize)) )
		{
			LogError(LOG_TRT "actionNet::PreProcess() -- cudaTensorNormMeanRGB() failed\n");
			return false;
		}
	}
	
	// update frame counters and pointers
	mInputs[0].CUDA = mInputBuffers[mCurrentInputBuffer];
	
	mCurrentInputBuffer = (mCurrentInputBuffer + 1) % 2;
	mCurrentFrameIndex += 1;
	
	if( mCurrentFrameIndex >= mNumFrames )
		mCurrentFrameIndex = mNumFrames - 1;

	PROFILER_END(PROFILER_PREPROCESS);
	return true;
}


// Process
bool actionNet::Process()
{
	PROFILER_BEGIN(PROFILER_NETWORK);

	if( !ProcessNetwork() )
		return false;

	PROFILER_END(PROFILER_NETWORK);
	return true;
}


// Classify
int actionNet::Classify( void* image, uint32_t width, uint32_t height, imageFormat format, float* confidence )
{
	// verify parameters
	if( !image || width == 0 || height == 0 )
	{
		LogError(LOG_TRT "actionNet::Classify( 0x%p, %u, %u ) -> invalid parameters\n", image, width, height);
		return -1;
	}
	
	// downsample and convert to band-sequential BGR
	if( !PreProcess(image, width, height, format) )
	{
		LogError(LOG_TRT "actionNet::Classify() -- tensor pre-processing failed\n");
		return -1;
	}
	
	return Classify(confidence);
}


// Classify
int actionNet::Classify( float* confidence )
{	
	// process with TRT
	if( !Process() )
	{
		LogError(LOG_TRT "actionNet::Process() failed\n");
		return -1;
	}
	
	PROFILER_BEGIN(PROFILER_POSTPROCESS);

	// determine the maximum class
	int classIndex = -1;
	float classMax = -1.0f;
	
	for( size_t n=0; n < mNumClasses; n++ )
	{
		const float value = mOutputs[0].CPU[n] * 0.01f;
		
		if( value >= 0.01f )
			LogVerbose("class %04zu - %f  (%s)\n", n, value, mClassDesc[n].c_str());
	
		if( value > classMax )
		{
			classIndex = n;
			classMax   = value;
		}
	}
	
	if( confidence != NULL )
		*confidence = classMax;
	
	//printf("\nmaximum class:  #%i  (%f) (%s)\n", classIndex, classMax, mClassDesc[classIndex].c_str());
	PROFILER_END(PROFILER_POSTPROCESS);	
	return classIndex;
}
