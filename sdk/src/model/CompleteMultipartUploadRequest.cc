/*
 * Copyright 2009-2017 Alibaba Cloud All rights reserved.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <alibabacloud/oss/model/CompleteMultipartUploadRequest.h>
#include <sstream>
#include "../utils/Utils.h"
#include "ModelError.h"

using namespace AlibabaCloud::OSS;

CompleteMultipartUploadRequest::CompleteMultipartUploadRequest(
    const std::string &bucket, const std::string &key) :
    CompleteMultipartUploadRequest(bucket, key, PartList())
{
}

CompleteMultipartUploadRequest::CompleteMultipartUploadRequest(
    const std::string &bucket,
    const std::string &key, const PartList &partList) :
    CompleteMultipartUploadRequest(bucket, key, partList, "")
{
}

CompleteMultipartUploadRequest::CompleteMultipartUploadRequest(
    const std::string &bucket, const std::string &key, 
    const PartList &partList, const std::string &uploadId):
    OssObjectRequest(bucket, key),
    partList_(partList),
    uploadId_(uploadId),
    encodingTypeIsSet_(false)
{
}

int CompleteMultipartUploadRequest::validate() const
{
    int ret = OssObjectRequest::validate();

    if (ret != 0) {
        return ret;
    }

    if(partList_.empty())
        return ARG_ERROR_MULTIPARTUPLOAD_PARTLIST_EMPTY;
    
    return 0;
}

void CompleteMultipartUploadRequest::setPartList(const PartList &partList)
{
    partList_ = partList;
}

void CompleteMultipartUploadRequest::setEncodingType(const std::string &encodingType)
{
    encodingType_ = encodingType;
    encodingTypeIsSet_ = true;
}

void CompleteMultipartUploadRequest::setUploadId(const std::string &uploadId)
{
    uploadId_ = uploadId;
}

ParameterCollection CompleteMultipartUploadRequest::specialParameters()const
{
    ParameterCollection parameters;
    parameters["uploadId"] = uploadId_;
    if(encodingTypeIsSet_)
    {
        parameters["encoding-type"] = encodingType_;
    }
    return parameters;
}

std::string CompleteMultipartUploadRequest::payload() const
{
    std::stringstream ss;
    ss << "<CompleteMultipartUpload>" << std::endl;
    for (auto const &part : partList_) {
        ss << "<Part>" << std::endl;
        ss << "  <PartNumber>";
        ss << part.PartNumber();
        ss << "</PartNumber>" << std::endl;
        ss << "  <ETag>";
        ss << part.ETag();
        ss << "</ETag>" << std::endl;
        ss << "</Part>";
    }
    ss << "</CompleteMultipartUpload>";
    return ss.str();
}
