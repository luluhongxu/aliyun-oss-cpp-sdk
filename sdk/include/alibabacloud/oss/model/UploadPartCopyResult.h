/*
 * Copyright 2009-2018 Alibaba Cloud All rights reserved.
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

#pragma once
#include <string>
#include <memory>
#include <alibabacloud/oss/Export.h>
#include <alibabacloud/oss/Types.h>
#include <alibabacloud/oss/OssResult.h>

namespace AlibabaCloud
{
namespace OSS
{
    class ALIBABACLOUD_OSS_EXPORT UploadPartCopyResult :public OssResult
    {
    public:
        UploadPartCopyResult();
        UploadPartCopyResult(const std::string& data);
        UploadPartCopyResult(const std::shared_ptr<std::iostream>& data,
             const HeaderCollection &header);
        UploadPartCopyResult& operator=(const std::string& data);
        const std::string& CopySourceIfMatch() const;
        const std::string& CopySourceIfNoneMatch() const;
        const std::string& CopySourceIfUnmodifiedSince() const;
        const std::string& CopySourceIfModifiedSince() const;
        const std::string& LastModified() const;
        const std::string& ETag() const;
     private:
        std::string sourceRange_;
        std::string lastModified_;
        std::string eTag_;
        HeaderCollection headers;
    };
}
}
