#!/usr/bin/env bash

# Figure out the paths.
script_path=`readlink -f $0`
script_dir=`dirname $script_path`
example_dir=`dirname $script_dir`
app_dir=`dirname $example_dir`

progname=caffe
prog_path=${app_dir}/build/tools/${progname}
servname=ps_server
serv_path=${app_dir}/build/tools/${servname}
mastname=ps_master
mast_path=${app_dir}/build/tools/${mastname}

host_filename="${app_dir}/machinefiles/localserver"
host_file=$(readlink -f $host_filename)

##=====================================
## Parameters
##=====================================

# Input files:

#dataset=alexnet
#solver_filename="${app_dir}/examples/test/solver.prototxt"

dataset=googlenet
solver_prefix="${app_dir}/examples/test_googlenet/quick_solver"
solver_postfix=".prototxt"

 # Uncomment this and line-93 if (re-)start training from a snapshot
#snapshot_filename="${app_dir}/(SOLVERSTATE_FILE)"

# System parameters:
svb=false
dwbp=true

##=====================================

ssh_options="-oStrictHostKeyChecking=no \
-oUserKnownHostsFile=/dev/null \
-oLogLevel=quiet"

# Parse hostfile
host_list=`cat $host_file | awk '{ print $2 }'`
unique_host_list=`cat $host_file | awk '{ print $2 }' | uniq`
num_unique_hosts=`cat $host_file | awk '{ print $2 }' | uniq | wc -l`
devices="0"

output_dir=$app_dir/output
output_dir="${output_dir}/caffe.${dataset}"
output_dir="${output_dir}.M${num_unique_hosts}"
if [ "$dwbp" = true ]; then
  output_dir="${output_dir}.D"
fi
if [ "$dwbp" = true ]; then
  output_dir="${output_dir}.S"
fi

log_dir=$output_dir/logs
net_outputs_prefix="${output_dir}/${dataset}"

# Kill previous instances of this program
echo "Killing previous instances of '$progname' on servers, please wait..."
for ip in $unique_host_list; do
  ssh $ssh_options $ip \
    killall -q $progname ps_master ps_server
done
echo "All done!"

sleep 3

# ------------- Start Program ------------- #

host_array=($unique_host_list)
mast_addr="tcp://${host_array[0]}:5555"

# Spawn program instances
client_id=0
caffe_cmd0=""
for ip in $unique_host_list; do
  echo Running client $client_id on $ip
  log_path=${log_dir}.${client_id}
  solver_filename=${solver_prefix}${client_id}${solver_postfix}

  cmd_prefix="'mkdir -p ${output_dir}; \
      mkdir -p ${log_path}; \
      export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib/:/usr/local/cuda-7.5/lib64/; \
      ulimit -c unlimited; \
      GLOG_logtostderr=false \
      GLOG_stderrthreshold=0 \
      GLOG_log_dir=$log_path \
      GLOG_v=-1 \
      GLOG_minloglevel=0 \
      GLOG_vmodule=""" # \

  server_cmd="$cmd_prefix \
      $serv_path tcp://${ip}:6666 ${mast_addr}'"

  caffe_cmd="$cmd_prefix \
      $prog_path train \
      --master_addr ${mast_addr}
      --client_id ${client_id} \
      --total_client_num ${#host_array[@]} \
      --solver=${solver_filename} \
      --svb=$svb \
      --dwbp=$dwbp \
      --net_outputs=${net_outputs_prefix} \
      --gpu=${devices} 2> ${log_dir}${client_id}'" #\
      #--gpu=${devices}'" #\
      #--gpu=${devices} 2> ${log_dir}${client_id}'" #\
      #--snapshot=${snapshot_filename}'"
  
  if [ $client_id -eq 0 ]; then
    master_cmd="$cmd_prefix \
        $mast_path ${mast_addr} ${#host_array[@]}'"
    ssh $ssh_options $ip bash -c $master_cmd &
  fi

  ssh $ssh_options $ip bash -c $server_cmd &
  ssh $ssh_options $ip bash -c $caffe_cmd &

  client_id=$(( client_id+1 ))
done

