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

#include <ctime>
#include <algorithm>
#include <sstream>
#include <set>
#include <tinyxml2/tinyxml2.h>
#include <alibabacloud/oss/http/HttpType.h>
#include "utils/Utils.h"
#include "utils/SignUtils.h"
#include "auth/HmacSha1Signer.h"
#include "OssClientImpl.h"
#include "../utils/LogUtils.h"

using namespace AlibabaCloud::OSS;
using namespace tinyxml2;

namespace
{
const std::string SERVICE_NAME = "OSS";
const char *TAG = "OssClientImpl";
}

OssClientImpl::OssClientImpl(const std::string &endpoint, const std::shared_ptr<CredentialsProvider>& credentialsProvider, const ClientConfiguration & configuration) :
    Client(SERVICE_NAME, configuration),
    endpoint_(endpoint),
    credentialsProvider_(credentialsProvider),
    signer_(std::make_shared<HmacSha1Signer>()),
    executor_(std::make_shared<Executor>())
{
}

OssClientImpl::~OssClientImpl()
{
}

int OssClientImpl::asyncExecute(Runnable * r) const
{
    if (executor_ == nullptr)
        return 1;
    
    executor_->execute(r);
    return 0;
}

std::shared_ptr<HttpRequest> OssClientImpl::buildHttpRequest(const std::string & endpoint, const ServiceRequest & msg, Http::Method method) const
{
    auto httpRequest = std::make_shared<HttpRequest>(method);
    auto calcContentMD5 = !!(msg.Flags()&REQUEST_FLAG_CONTENTMD5);
    auto paramInPath = !!(msg.Flags()&REQUEST_FLAG_PARAM_IN_PATH);
    httpRequest->setResponseStreamFactory(msg.ResponseStreamFactory());
    addHeaders(httpRequest, msg.Headers());
    addBody(httpRequest, msg.Body(), calcContentMD5);
    if (paramInPath) {
        httpRequest->setUrl(Url(msg.Path()));
    }
    else {
        addSignInfo(httpRequest, msg);
        addUrl(httpRequest, endpoint, msg);
    }
    addOther(httpRequest, msg);
    return httpRequest;
}

bool OssClientImpl::hasResponseError(const std::shared_ptr<HttpResponse>&response) const
{
    if (BASE::hasResponseError(response)) {
        return true;
    }

    //check crc64
    if (response->request().hasCheckCrc64() && 
        response->hasHeader("x-oss-hash-crc64ecma")) {
        uint64_t clientCrc64 = response->request().Crc64Result();
        uint64_t serverCrc64 = std::strtoull(response->Header("x-oss-hash-crc64ecma").c_str(), nullptr, 10);
        if (clientCrc64 != serverCrc64) {
            response->setStatusCode(ERROR_CRC_INCONSISTENT);
            std::stringstream ss;
            ss << "Crc64 validation failed. Expected hash:" << serverCrc64
                << " not equal to calculated hash:" << clientCrc64
                << ". Transferd bytes:" << response->request().TransferedBytes()
                << ". RequestId:" << response->Header("x-oss-request-id").c_str();
            response->setStatusMsg(ss.str().c_str());
            return true;
        }
    }
    return false;
}


void OssClientImpl::addHeaders(const std::shared_ptr<HttpRequest> &httpRequest, const HeaderCollection &headers) const
{
    for (auto const& header : headers) {
        httpRequest->addHeader(header.first, header.second);
    }

    //common headers
    httpRequest->addHeader(Http::USER_AGENT, configuration().userAgent);

    //Date
    if (!httpRequest->hasHeader(Http::DATE)) {
        std::time_t t = std::time(nullptr);
        httpRequest->addHeader(Http::DATE, ToGmtTime(t));
    }
}

void OssClientImpl::addBody(const std::shared_ptr<HttpRequest> &httpRequest, const std::shared_ptr<std::iostream>& body, bool contentMd5) const
{
    if (body == nullptr) {
        Http::Method methold = httpRequest->method();
        if (methold == Http::Method::Get || methold == Http::Method::Post) {
            httpRequest->setHeader(Http::CONTENT_LENGTH, "0");
        } else {
            httpRequest->removeHeader(Http::CONTENT_LENGTH);
        }
    }
    
    if ((body != nullptr) && !httpRequest->hasHeader(Http::CONTENT_LENGTH)) {
        auto streamSize = GetIOStreamLength(*body);
        httpRequest->setHeader(Http::CONTENT_LENGTH, std::to_string(streamSize));
    }

    if (contentMd5 && body && !httpRequest->hasHeader(Http::CONTENT_MD5)) {
        auto md5 = ComputeContentMD5(*body);
        httpRequest->setHeader(Http::CONTENT_MD5, md5);
    }

    httpRequest->addBody(body);
}

void OssClientImpl::addSignInfo(const std::shared_ptr<HttpRequest> &httpRequest, const ServiceRequest &request) const
{
    const Credentials credentials = credentialsProvider_->getCredentials();
    if (!credentials.SessionToken().empty()) {
        httpRequest->addHeader("x-oss-security-token", credentials.SessionToken());
    }

    //Sort the parameters
    ParameterCollection parameters;
    for (auto const&param : request.Parameters()) {
        parameters[param.first] = param.second;
    }

    std::string method = Http::MethodToString(httpRequest->method());

    const OssRequest& ossRequest = static_cast<const OssRequest&>(request);
    std::string resource;
    resource.append("/");
    if (!ossRequest.bucket().empty()) {
        resource.append(ossRequest.bucket());
        resource.append("/");
    }
    if (!ossRequest.key().empty()) {
        resource.append(ossRequest.key());
    }

    std::string date = httpRequest->Header(Http::DATE);

    SignUtils signUtils(signer_->version());
    signUtils.build(method, resource, date, httpRequest->Headers(), parameters);
    auto signature = signer_->generate(signUtils.CanonicalString(), credentials.AccessKeySecret());

    std::stringstream authValue;
    authValue
        << "OSS "
        << credentials.AccessKeyId()
        << ":"
        << signature;

    httpRequest->addHeader(Http::AUTHORIZATION, authValue.str());

    OSS_LOG(LogLevel::LogDebug, TAG, "client(%p) request(%p) CanonicalString:%s", this, httpRequest.get(), signUtils.CanonicalString().c_str());
    OSS_LOG(LogLevel::LogDebug, TAG, "client(%p) request(%p) Authorization:%s", this, httpRequest.get(), authValue.str().c_str());
}

void OssClientImpl::addUrl(const std::shared_ptr<HttpRequest> &httpRequest, const std::string &endpoint, const ServiceRequest &request) const
{
    const OssRequest& ossRequest = static_cast<const OssRequest&>(request);

    auto host = CombineHostString(endpoint, ossRequest.bucket(), configuration().isCname);
    auto path = CombinePathString(endpoint, ossRequest.bucket(), ossRequest.key());

    Url url(host);
    url.setPath(path);

    auto parameters = request.Parameters();
    if (!parameters.empty()) {
        std::stringstream queryString;
        for (const auto &p : parameters)
        {
            if (p.second.empty())
                queryString << "&" << UrlEncode(p.first);
            else
                queryString << "&" << UrlEncode(p.first) << "=" << UrlEncode(p.second);
        }
        url.setQuery(queryString.str().substr(1));
    }
    httpRequest->setUrl(url);
}

void OssClientImpl::addOther(const std::shared_ptr<HttpRequest> &httpRequest, const ServiceRequest &request) const
{
    //progress
    httpRequest->setTransferProgress(request.TransferProgress());

    //crc64 check
    auto checkCRC64 = !!(request.Flags()&REQUEST_FLAG_CHECK_CRC64);
    if (configuration().enableCrc64 &&
        checkCRC64 &&
        !httpRequest->hasHeader(Http::RANGE)) {
        httpRequest->setCheckCrc64(true);
#ifdef ENABLE_OSS_TEST
        if (!!(request.Flags()&0x80000000)) {
            httpRequest->addHeader("oss-test-crc64", "1");
        }
#endif
    }
}

OssError OssClientImpl::buildError(const Error &error) const
{
    OssError err;
    if (error.Status() > 299 && error.Status() < 600 && !error.Message().empty()) {
        XMLDocument doc;
        XMLError xml_err;
        if ((xml_err = doc.Parse(error.Message().c_str(), error.Message().size())) == XML_SUCCESS) {
            XMLElement* root =doc.RootElement();
            if (root && !std::strncmp("Error", root->Name(), 5)) {
                XMLElement *node;
                node = root->FirstChildElement("Code");
                err.setCode(node ? node->GetText(): "");
                node = root->FirstChildElement("Message");
                err.setMessage(node ? node->GetText(): "");
                node = root->FirstChildElement("RequestId");
                err.setRequestId(node ? node->GetText(): "");
                node = root->FirstChildElement("HostId");
                err.setHost(node ? node->GetText(): "");
            } else {
                err.setCode("ParseXMLError");
                err.setMessage("Xml format invalid, root node name is not Error. the content is:\n" + error.Message());
            }
        } else {
            std::stringstream ss;
            ss << "ParseXMLError:" << xml_err;
            err.setCode(ss.str());
            err.setMessage(XMLDocument::ErrorIDToName(xml_err));
        }
    } 
    else {
        err.setCode(error.Code());
        err.setMessage(error.Message());
    }

    //get from header if body has nothing
    if (err.RequestId().empty()) {
        auto it = error.Headers().find("x-oss-request-id");
        if (it != error.Headers().end()) {
            err.setRequestId(it->second);
        }
    }

    return err;
}

ServiceResult OssClientImpl::buildResult(const std::shared_ptr<HttpResponse> &httpResponse) const
{
    ServiceResult result;
    result.setRequestId(httpResponse->Header("x-oss-request-id"));
    result.setPlayload(httpResponse->Body());
    result.setResponseCode(httpResponse->statusCode());
    result.setHeaderCollection(httpResponse->Headers());
    return result;
}

OssOutcome OssClientImpl::MakeRequest(const OssRequest &request, Http::Method method) const
{
    int ret = request.validate();
    if (ret != 0) {
        return OssOutcome(OssError("ValidateError", request.validateMessage(ret)));
    }

    auto outcome = BASE::AttemptRequest(endpoint_, request, method);
    if (outcome.isSuccess()) {
        return OssOutcome(buildResult(outcome.result()));
    } else {
        return OssOutcome(buildError(outcome.error()));
    }
}

ListBucketsOutcome OssClientImpl::ListBuckets(const ListBucketsRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        ListBucketsResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? ListBucketsOutcome(std::move(result)) :
            ListBucketsOutcome(OssError("ParseXMLError", "Parsing ListBuckets result fail."));
    } else {
        return ListBucketsOutcome(outcome.error());
    }
}

CreateBucketOutcome OssClientImpl::CreateBucket(const CreateBucketRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        return  CreateBucketOutcome(Bucket());
    } else {
        return CreateBucketOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketAcl(const SetBucketAclRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    } else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketLogging(const SetBucketLoggingRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketWebsite(const SetBucketWebsiteRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketReferer(const SetBucketRefererRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketLifecycle(const SetBucketLifecycleRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketCors(const SetBucketCorsRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetBucketStorageCapacity(const SetBucketStorageCapacityRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::DeleteBucket(const DeleteBucketRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Delete);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::DeleteBucketLogging(const DeleteBucketLoggingRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Delete);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::DeleteBucketWebsite(const DeleteBucketWebsiteRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Delete);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::DeleteBucketLifecycle(const DeleteBucketLifecycleRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Delete);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::DeleteBucketCors(const DeleteBucketCorsRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Delete);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

ListObjectOutcome OssClientImpl::ListObjects(const ListObjectsRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        ListObjectsResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? ListObjectOutcome(std::move(result)) :
            ListObjectOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return ListObjectOutcome(outcome.error());
    }
}

GetBucketAclOutcome OssClientImpl::GetBucketAcl(const GetBucketAclRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketAclResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketAclOutcome(std::move(result)) :
            GetBucketAclOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketAclOutcome(outcome.error());
    }
}

GetBucketLocationOutcome OssClientImpl::GetBucketLocation(const GetBucketLocationRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketLocationResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketLocationOutcome(std::move(result)) :
            GetBucketLocationOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketLocationOutcome(outcome.error());
    }
}

GetBucketInfoOutcome  OssClientImpl::GetBucketInfo(const  GetBucketInfoRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketInfoResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketInfoOutcome(std::move(result)) :
            GetBucketInfoOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketInfoOutcome(outcome.error());
    }
}

GetBucketLoggingOutcome OssClientImpl::GetBucketLogging(const GetBucketLoggingRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketLoggingResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketLoggingOutcome(std::move(result)) :
            GetBucketLoggingOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketLoggingOutcome(outcome.error());
    }
}

GetBucketWebsiteOutcome OssClientImpl::GetBucketWebsite(const GetBucketWebsiteRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketWebsiteResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketWebsiteOutcome(std::move(result)) :
            GetBucketWebsiteOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketWebsiteOutcome(outcome.error());
    }
}

GetBucketRefererOutcome OssClientImpl::GetBucketReferer(const GetBucketRefererRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketRefererResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketRefererOutcome(std::move(result)) :
            GetBucketRefererOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketRefererOutcome(outcome.error());
    }
}

GetBucketLifecycleOutcome OssClientImpl::GetBucketLifecycle(const GetBucketLifecycleRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketLifecycleResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketLifecycleOutcome(std::move(result)) :
            GetBucketLifecycleOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketLifecycleOutcome(outcome.error());
    }
}

GetBucketStatOutcome OssClientImpl::GetBucketStat(const GetBucketStatRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketStatResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketStatOutcome(std::move(result)) :
            GetBucketStatOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketStatOutcome(outcome.error());
    }
}

GetBucketCorsOutcome OssClientImpl::GetBucketCors(const GetBucketCorsRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketCorsResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketCorsOutcome(std::move(result)) :
            GetBucketCorsOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketCorsOutcome(outcome.error());
    }
}

GetBucketStorageCapacityOutcome OssClientImpl::GetBucketStorageCapacity(const GetBucketStorageCapacityRequest& request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetBucketStorageCapacityResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetBucketStorageCapacityOutcome(std::move(result)) :
            GetBucketStorageCapacityOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetBucketStorageCapacityOutcome(outcome.error());
    }
}

#undef GetObject
GetObjectOutcome OssClientImpl::GetObject(const GetObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        return GetObjectOutcome(GetObjectResult(request.Bucket(), request.Key(),
            outcome.result().payload(),outcome.result().headerCollection()));
    }
    else {
        return GetObjectOutcome(outcome.error());
    }
}

PutObjectOutcome OssClientImpl::PutObject(const PutObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        const HeaderCollection& header = outcome.result().headerCollection();
        PutObjectResult result(header);
        result.requestId_ = outcome.result().RequestId();
        return PutObjectOutcome(result);
    }
    else {
        return PutObjectOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::DeleteObject(const DeleteObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Delete);
    if (outcome.isSuccess()) {
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

DeleteObjecstOutcome OssClientImpl::DeleteObjects(const DeleteObjectsRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Post);
    if (outcome.isSuccess()) {
        DeleteObjectsResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? DeleteObjecstOutcome(std::move(result)) :
            DeleteObjecstOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return DeleteObjecstOutcome(outcome.error());
    }
}

ObjectMetaDataOutcome OssClientImpl::HeadObject(const HeadObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Head);
    if (outcome.isSuccess()) {
        ObjectMetaData metaData = outcome.result().headerCollection();
        return ObjectMetaDataOutcome(std::move(metaData));
    }
    else {
        return ObjectMetaDataOutcome(outcome.error());
    }
}


ObjectMetaDataOutcome OssClientImpl::GetObjectMeta(const GetObjectMetaRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Head);
    if (outcome.isSuccess()) {
        ObjectMetaData metaData = outcome.result().headerCollection();
        return ObjectMetaDataOutcome(std::move(metaData));
    }
    else {
        return ObjectMetaDataOutcome(outcome.error());
    }
}

GetObjectAclOutcome OssClientImpl::GetObjectAcl(const GetObjectAclRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        GetObjectAclResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? GetObjectAclOutcome(std::move(result)) :
            GetObjectAclOutcome(OssError("ParseXMLError", "Parsing ListObject result fail."));
    }
    else {
        return GetObjectAclOutcome(outcome.error());
    }
}

AppendObjectOutcome OssClientImpl::AppendObject(const AppendObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Post);
    if (outcome.isSuccess()) {
        const HeaderCollection& header = outcome.result().headerCollection();
		AppendObjectResult result(header);
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? AppendObjectOutcome(std::move(result)) :
            AppendObjectOutcome(OssError("ParseXMLError", "no position or no crc64"));
    }
    else {
        return AppendObjectOutcome(outcome.error());
    }
}

CopyObjectOutcome OssClientImpl::CopyObject(const CopyObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        CopyObjectResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return CopyObjectOutcome(std::move(result));
    }
    else {
        return CopyObjectOutcome(outcome.error());
    }
}

GetSymlinkOutcome OssClientImpl::GetSymlink(const GetSymlinkRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Get);
    if (outcome.isSuccess()) {
        const HeaderCollection& header = outcome.result().headerCollection();
        GetSymlinkResult result(header.at("x-oss-symlink-target")
                                  ,header.at("ETag"));
        result.requestId_ = outcome.result().RequestId();
        return GetSymlinkOutcome(std::move(result));
    }
    else {
        return GetSymlinkOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::RestoreObject(const RestoreObjectRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Post);
    if (outcome.isSuccess()) {
		VoidResult result;
		result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(std::move(result));
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

CreateSymlinkOutcome OssClientImpl::CreateSymlink(const CreateSymlinkRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
        const HeaderCollection& header = outcome.result().headerCollection();
        CreateSymlinkResult result(header.at("ETag"));
        result.requestId_ = outcome.result().RequestId();
        return CreateSymlinkOutcome(std::move(result));
    }
    else {
        return CreateSymlinkOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::SetObjectAcl(const SetObjectAclRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Method::Put);
    if (outcome.isSuccess()) {
		VoidResult result;
		result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(std::move(result));
    }
    else {
        return VoidOutcome(outcome.error());
    }
}


StringOutcome OssClientImpl::GeneratePresignedUrl(const GeneratePresignedUrlRequest &request) const
{
    if (!IsValidBucketName(request.bucket_) ||
        !IsValidObjectKey(request.key_)) {
        return StringOutcome(OssError("ValidateError", "The Bucket or Key is invalid."));
    }

    HeaderCollection headers;
    for (auto const&header : request.metaData_.HttpMetaData()) {
        headers[header.first] = header.second;
    }
    for (auto const&header : request.metaData_.UserMetaData()) {
        std::string key("x-oss-meta-");
        key.append(header.first);
        headers[key] = header.second;
    }

    ParameterCollection parameters;
    const Credentials credentials = credentialsProvider_->getCredentials();
    if (!credentials.SessionToken().empty()) {
        parameters["security-token"] = credentials.SessionToken();
    }
    for (auto const&param : request.parameters_) {
        parameters[param.first] = param.second;
    }

    SignUtils signUtils(signer_->version());
    auto method = Http::MethodToString(request.method_);
    auto resource = std::string().append("/").append(request.bucket_).append("/").append(request.key_);
    auto date = headers[Http::EXPIRES];
    signUtils.build(method, resource, date, headers, parameters);
    auto signature = signer_->generate(signUtils.CanonicalString(), credentials.AccessKeySecret());
    parameters["Expires"] = date;
    parameters["OSSAccessKeyId"] = credentials.AccessKeyId();
    parameters["Signature"] = signature;

    std::stringstream ss;
    ss << CombineHostString(endpoint_, request.bucket_, configuration().isCname);
    ss << CombinePathString(endpoint_, request.bucket_, request.key_);
    ss << "?";
    ss << CombineQueryString(parameters);

    return StringOutcome(ss.str());
}

GetObjectOutcome OssClientImpl::GetObjectByUrl(const GetObjectByUrlRequest &request) const
{
    auto outcome = BASE::AttemptRequest(endpoint_, request, Http::Method::Get);
    if (outcome.isSuccess()) {
        return GetObjectOutcome(GetObjectResult("", "", 
            outcome.result()->Body(),
            outcome.result()->Headers()));
    }
    else {
        return GetObjectOutcome(buildError(outcome.error()));
    }
}

PutObjectOutcome OssClientImpl::PutObjectByUrl(const PutObjectByUrlRequest &request) const
{
    auto outcome = BASE::AttemptRequest(endpoint_, request, Http::Method::Put);
    if (outcome.isSuccess()) {
        const HeaderCollection& header = outcome.result()->Headers();
        return PutObjectOutcome(PutObjectResult(header));
    }
    else {
        return PutObjectOutcome(buildError(outcome.error()));
    }
}

InitiateMultipartUploadOutcome OssClientImpl::InitiateMultipartUpload(const InitiateMultipartUploadRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Post);
    if(outcome.isSuccess()){
        InitiateMultipartUploadResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? InitiateMultipartUploadOutcome(std::move(result)):
                InitiateMultipartUploadOutcome(
                    OssError("InitiateMultipartUploadError",
                    "Parsing InitiateMultipartUploadResult fail"));
    }
    else{
        return InitiateMultipartUploadOutcome(outcome.error());
    }
}

PutObjectOutcome OssClientImpl::UploadPart(const UploadPartRequest &request)const
{
    auto outcome = MakeRequest(request, Http::Put);
    if(outcome.isSuccess()){
        const HeaderCollection& header = outcome.result().headerCollection();
        return PutObjectOutcome(PutObjectResult(header));
    }else{
        return PutObjectOutcome(outcome.error());
    }
}

UploadPartCopyOutcome OssClientImpl::UploadPartCopy(const UploadPartCopyRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Put);
    if(outcome.isSuccess()){
        const HeaderCollection& header = outcome.result().headerCollection();
        return UploadPartCopyOutcome(
            UploadPartCopyResult(outcome.result().payload(), header));
    }
    else{
        return UploadPartCopyOutcome(outcome.error());
    }
}

CompleteMultipartUploadOutcome OssClientImpl::CompleteMultipartUpload(const CompleteMultipartUploadRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Post);
    if (outcome.isSuccess()){
        CompleteMultipartUploadResult result(outcome.result().payload(), outcome.result().headerCollection());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ?
            CompleteMultipartUploadOutcome(std::move(result)) : 
            CompleteMultipartUploadOutcome(OssError("CompleteMultipartUpload", ""));
    }
    else {
        return CompleteMultipartUploadOutcome(outcome.error());
    }
}

VoidOutcome OssClientImpl::AbortMultipartUpload(const AbortMultipartUploadRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Delete);
    if(outcome.isSuccess()){
        VoidResult result;
        result.requestId_ = outcome.result().RequestId();
        return VoidOutcome(result);
    }
    else {
        return VoidOutcome(outcome.error());
    }
}

ListMultipartUploadsOutcome OssClientImpl::ListMultipartUploads(
    const ListMultipartUploadsRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Get);
    if(outcome.isSuccess())
    {
        ListMultipartUploadsResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ?
            ListMultipartUploadsOutcome(std::move(result)) :
            ListMultipartUploadsOutcome(OssError("ListMultipartUploads", "Parse Error"));
    }
    else {
        return ListMultipartUploadsOutcome(outcome.error());
    }
}

ListPartsOutcome OssClientImpl::ListParts(const ListPartsRequest &request) const
{
    auto outcome = MakeRequest(request, Http::Get);
    if(outcome.isSuccess())
    {
        ListPartsResult result(outcome.result().payload());
        result.requestId_ = outcome.result().RequestId();
        return result.ParseDone() ? 
            ListPartsOutcome(std::move(result)) :
            ListPartsOutcome(OssError("ListParts", "Parse Error"));
    }else{
        return ListPartsOutcome(outcome.error());
    }
}

/*Requests control*/
void OssClientImpl::DisableRequest()
{
    BASE::disableRequest();
    OSS_LOG(LogLevel::LogDebug, TAG, "client(%p) DisableRequest", this);
}

void OssClientImpl::EnableRequest()
{
    BASE::enableRequest();
    OSS_LOG(LogLevel::LogDebug, TAG, "client(%p) EnableRequest", this);
}

