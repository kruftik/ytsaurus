#!/bin/bash

set -e
set -x

script_name=$0
output_path="."
ytsaurus_source_path="."
ytsaurus_build_path="."
image_tag="latest"
image_cr=""

print_usage() {
    cat << EOF
Usage: $script_name [-h|--help]
                    [--ytsaurus-source-path /path/to/ytsaurus.repo (default: $ytsaurus_source_path)]
                    [--ytsaurus-build-path /path/to/ytsaurus.build (default: $ytsaurus_build_path)]
                    [--output-path /path/to/output (default: $output_path)]
                    [--image-tag some-tag (default: $image_tag)]
                    [--image-cr some-cr/ (default: $image_cr)]
EOF
    exit 1
}

# Parse options
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        --ytsaurus-source-path)
        ytsaurus_source_path="$2"
        shift 2
        ;;
        --ytsaurus-build-path)
        ytsaurus_build_path="$2"
        shift 2
        ;;
        --output-path)
        output_path="$2"
        shift 2
        ;;
        --image-tag)
        image_tag="$2"
        shift 2
        ;;
        --image-cr)
        image_cr="$2"
        shift 2
        ;;
        -h|--help)
        print_usage
        shift
        ;;
        *)  # unknown option
        echo "Unknown argument $1"
        print_usage
        ;;
    esac
done


systest="${ytsaurus_build_path}/yt/systest/bin/systest"
dockerfile="${ytsaurus_source_path}/yt/docker/systest/Dockerfile"

cp ${systest} ${output_path}
cp ${dockerfile} ${output_path}
cp -r ${ytsaurus_build_path}/ytsaurus_python ${output_path}
cp -r ${ytsaurus_source_path}/yt/yt/experiments/public/new_stress_test/ ${output_path}
cp ${ytsaurus_source_path}/yt/systest/scripts/init.sh ${output_path}

cd ${output_path}

docker build -t ${image_cr}ytsaurus/ytsaurus-systest:${image_tag} .
