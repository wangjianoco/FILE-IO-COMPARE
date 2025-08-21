#!/bin/bash

SRC_FILE="./test_src.bin"
DST_FILE="./test_des.bin"
OUTPUT_DIR="./io_test_results"

# 创建结果目录
mkdir -p $OUTPUT_DIR

# 测试程序列表
PROGRAMS=("dd1M_directio" "dd1M_buffferio" "read_write_bufferio" "read_write_directio" "io_uring_bufferio" "io_uring_directio" "splice_test") 

echo "Starting I/O performance tests..."
echo "Start time: $(date)"

for prog in "${PROGRAMS[@]}"; do
    RESULT_FILE="$OUTPUT_DIR/${prog}_result.txt"
    TOP_FILE="$OUTPUT_DIR/${prog}_top.txt"

    # 清空旧文件
    > $RESULT_FILE
    > $TOP_FILE

    echo "Running $prog ..."

    # 启动 top 监控，每秒记录一次，后台运行
    {
        top -b -d 1 -n 9999 | grep %Cpu >> $TOP_FILE
    } &
    TOP_PID=$!

    # 根据程序名称选择运行命令
    case $prog in
        dd1M_directio)
            (time dd if=$SRC_FILE of=$DST_FILE bs=1M count=1024 conv=fsync oflag=direct iflag=direct) >> $RESULT_FILE 2>&1
            ;;
        dd1M_buffferio)
            (time dd if=$SRC_FILE of=$DST_FILE bs=1M count=1024 conv=fsync) >> $RESULT_FILE 2>&1
            ;;

        *)
            (time ./$prog $SRC_FILE $DST_FILE) >> $RESULT_FILE 2>&1
            ;;
    esac

    # 结束 top 监控
    kill $TOP_PID

    echo "$prog done. Result saved to $RESULT_FILE, top info saved to $TOP_FILE"
    sleep 5
done

echo "All I/O tests completed at $(date)"