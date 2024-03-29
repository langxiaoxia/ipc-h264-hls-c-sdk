#!/bin/bash

last_key=`aws s3 ls s3://$AWS_S3_BUCKET/$AWS_S3_PREFIX --recursive | sort | tail -n 1 | awk '{print $4}'`
last_seq=`aws s3api head-object --bucket ${AWS_S3_BUCKET} --key $last_key | grep "\"seq\":" | awk '{print $2}'`
echo $last_seq
