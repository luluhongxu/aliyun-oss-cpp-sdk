#include <alibabacloud/oss/OssClient.h>
#include <iostream>
#include "Config.h"
#include "service/ServiceSample.h"
#include "bucket/BucketSample.h"
#include "object/ObjectSample.h"
#include "presignedurl/PresignedUrlSample.h"

using namespace AlibabaCloud::OSS;

void LogCallbackFunc(LogLevel level, const std::string &stream)
{
    if (level == LogLevel::LogOff)
        return;

    std::cout << stream;
}

int main(void)
{
    std::cout << "oss-cpp-sdk samples" << std::endl;
    std::string bucketName = "<YourBucketName>";

    InitializeSdk();

    SetLogLevel(LogLevel::LogDebug);
    SetLogCallback(LogCallbackFunc);

    ServiceSample serviceSample;
    serviceSample.ListBuckets();
    serviceSample.ListBucketsWithMarker();
    serviceSample.ListBucketsWithPrefix();

    BucketSample bucketSample(bucketName);
    bucketSample.InvalidBucketName();
    bucketSample.CreateAndDeleteBucket();
    bucketSample.SetBucketAcl();
    bucketSample.SetBucketLogging();
    bucketSample.SetBucketWebsite();
    bucketSample.SetBucketReferer();
    bucketSample.SetBucketLifecycle();
    bucketSample.SetBucketCors();
    bucketSample.GetBucketCors();

    bucketSample.DeleteBucketLogging();
    bucketSample.DeleteBucketWebsite();
    bucketSample.DeleteBucketLifecycle();
    bucketSample.DeleteBucketCors();


    bucketSample.ListObjects();
    bucketSample.ListObjectWithMarker();
    bucketSample.ListObjectWithEncodeType();

    bucketSample.GetBucketAcl();
    bucketSample.GetBucketLocation();
    bucketSample.GetBucketLogging();
    bucketSample.GetBucketWebsite();
    bucketSample.GetBucketReferer();
    bucketSample.GetBucketStat();
    bucketSample.GetBucketLifecycle();
    //bucketSample.DeleteBucketsByPrefix();


    ObjectSample objectSample(bucketName);
    objectSample.PutObjectFromBuffer();
    objectSample.PutObjectFromFile();
    objectSample.GetObjectToBuffer();
    objectSample.GetObjectToFile();
    objectSample.DeleteObject();
    objectSample.DeleteObjects();
    objectSample.HeadObject();
    objectSample.GetObjectMeta();
    objectSample.AppendObject();
    objectSample.PutObjectProgress();
    objectSample.GetObjectProgress();
    objectSample.PutObjectCallable();
    objectSample.GetObjectCallable();
    objectSample.CopyObject();
    //objectSample.RestoreArchiveObject("your-archive", "oss_archive_object.PNG", 1);

    PresignedUrlSample signedUrlSample(bucketName);
    signedUrlSample.GenGetPresignedUrl();
    signedUrlSample.PutObjectByUrlFromBuffer();
    signedUrlSample.PutObjectByUrlFromFile();
    signedUrlSample.GetObjectByUrlToBuffer();
    signedUrlSample.GetObjectByUrlToFile();

    ShutdownSdk();
    return 1;
}
