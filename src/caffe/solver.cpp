#include <cstdio>

#include <string>
#include <vector>
#include <thread>

#include "caffe/solver.hpp"
#include "caffe/util/format.hpp"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/upgrade_proto.hpp"
#include "caffe/util/benchmark.hpp"

namespace caffe {

template<typename Dtype>
void Solver<Dtype>::SetActionFunction(ActionCallback func) {
  action_request_function_ = func;
}

template<typename Dtype>
SolverAction::Enum Solver<Dtype>::GetRequestedAction() {
  if (action_request_function_) {
    // If the external request function has been set, call it.
    return action_request_function_();
  }
  return SolverAction::NONE;
}

template <typename Dtype>
Solver<Dtype>::Solver(const SolverParameter& param, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false) {
  Init(param);
}

template <typename Dtype>
Solver<Dtype>::Solver(const string& param_file, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false) {
  SolverParameter param;
  ReadSolverParamsFromTextFileOrDie(param_file, &param);
  Init(param);
}

template <typename Dtype>
void Solver<Dtype>::Init(const SolverParameter& param) {
  CHECK(Caffe::root_solver() || root_solver_)
      << "root_solver_ needs to be set for all non-root solvers";
  LOG_IF(INFO, Caffe::root_solver()) << "Initializing solver from parameters: "
    << std::endl << param.DebugString();
  param_ = param;
  CHECK_GE(param_.average_loss(), 1) << "average_loss should be non-negative.";
  CheckSnapshotWritePermissions();
  if (Caffe::root_solver() && param_.random_seed() >= 0) {
    Caffe::set_random_seed(param_.random_seed());
  }
  // Scaffolding code
  InitTrainNet();
  if (Caffe::root_solver()) {
    InitTestNets();
    LOG(INFO) << "Solver scaffolding done.";
  }
  iter_ = 0;
  current_step_ = 0;

  // -------------- PS --------------
  auto learnable_params = net_->learnable_params();

  for (int i = 0; i < learnable_params.size(); ++i) {
    std::shared_ptr<Blob<Dtype> > 
      my_bf(new Blob<Dtype>(learnable_params[i]->shape()));
    ps_buffer_.push_back(my_bf);
  }
  // vector<int> shape {count};
  // ps_buffer_.reset(new Blob<Dtype>(shape));
  // LOG(INFO) << "Init PSBUFFER " << count;

  for (int i = 0; i < learnable_params.size(); ++i) {
    send_.push_back(std::shared_ptr<ps::Send<Dtype>>
      (new ps::Send<Dtype>(Caffe::master_addr(), i)));
  }

  irecvall_.reset(new ps::IRecvAll<Dtype>(Caffe::recv_addr(), Caffe::master_addr()));

  vector<int> keys;
  for (int i = 0; i < learnable_params.size(); ++i) {
    keys.push_back(i);
  }
  wait_.reset(new ps::Wait<Dtype>(keys));

  if (Caffe::client_id() == 0) {
    LOG(INFO) << "Upload inited params to ps --------";
    for (int i = 0; i < learnable_params.size(); ++i) {
      AsyncGradGPUs(i, true);
    }
  } else {
    // BUG
    LOG(INFO) << "Download inited params from ps --------";
    for (int i = 0; i < learnable_params.size(); ++i) {
      int key = i;
      int size = ps_buffer_[i]->count();

      Dtype* ps_bf_data = ps_buffer_[i]->mutable_cpu_data();
      wait_->Run(ps_bf_data, size*sizeof(Dtype), key);

      Dtype* gpu_data = net_->learnable_params()[i]->mutable_gpu_data();
      cudaMemcpy(gpu_data, ps_bf_data, size*sizeof(Dtype), cudaMemcpyHostToDevice);
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::InitTrainNet() {
  const int num_train_nets = param_.has_net() + param_.has_net_param() +
      param_.has_train_net() + param_.has_train_net_param();
  const string& field_names = "net, net_param, train_net, train_net_param";
  CHECK_GE(num_train_nets, 1) << "SolverParameter must specify a train net "
      << "using one of these fields: " << field_names;
  CHECK_LE(num_train_nets, 1) << "SolverParameter must not contain more than "
      << "one of these fields specifying a train_net: " << field_names;
  NetParameter net_param;
  if (param_.has_train_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in train_net_param.";
    net_param.CopyFrom(param_.train_net_param());
  } else if (param_.has_train_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from train_net file: " << param_.train_net();
    ReadNetParamsFromTextFileOrDie(param_.train_net(), &net_param);
  }
  if (param_.has_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in net_param.";
    net_param.CopyFrom(param_.net_param());
  }
  if (param_.has_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from net file: " << param_.net();
    ReadNetParamsFromTextFileOrDie(param_.net(), &net_param);
  }
  // Set the correct NetState.  We start with the solver defaults (lowest
  // precedence); then, merge in any NetState specified by the net_param itself;
  // finally, merge in any NetState specified by the train_state (highest
  // precedence).
  NetState net_state;
  net_state.set_phase(TRAIN);
  net_state.MergeFrom(net_param.state());
  net_state.MergeFrom(param_.train_state());
  net_param.mutable_state()->CopyFrom(net_state);
  if (Caffe::root_solver()) {
    net_.reset(new Net<Dtype>(net_param));
  } else {
    net_.reset(new Net<Dtype>(net_param, root_solver_->net_.get()));
  }
}

template <typename Dtype>
void Solver<Dtype>::InitTestNets() {
  CHECK(Caffe::root_solver());
  const bool has_net_param = param_.has_net_param();
  const bool has_net_file = param_.has_net();
  const int num_generic_nets = has_net_param + has_net_file;
  CHECK_LE(num_generic_nets, 1)
      << "Both net_param and net_file may not be specified.";
  const int num_test_net_params = param_.test_net_param_size();
  const int num_test_net_files = param_.test_net_size();
  const int num_test_nets = num_test_net_params + num_test_net_files;
  if (num_generic_nets) {
      CHECK_GE(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  } else {
      CHECK_EQ(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  }
  // If we have a generic net (specified by net or net_param, rather than
  // test_net or test_net_param), we may have an unlimited number of actual
  // test networks -- the actual number is given by the number of remaining
  // test_iters after any test nets specified by test_net_param and/or test_net
  // are evaluated.
  const int num_generic_net_instances = param_.test_iter_size() - num_test_nets;
  const int num_test_net_instances = num_test_nets + num_generic_net_instances;
  if (param_.test_state_size()) {
    CHECK_EQ(param_.test_state_size(), num_test_net_instances)
        << "test_state must be unspecified or specified once per test net.";
  }
  if (num_test_net_instances) {
    CHECK_GT(param_.test_interval(), 0);
  }
  int test_net_id = 0;
  vector<string> sources(num_test_net_instances);
  vector<NetParameter> net_params(num_test_net_instances);
  for (int i = 0; i < num_test_net_params; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net_param";
      net_params[test_net_id].CopyFrom(param_.test_net_param(i));
  }
  for (int i = 0; i < num_test_net_files; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net file: " + param_.test_net(i);
      ReadNetParamsFromTextFileOrDie(param_.test_net(i),
          &net_params[test_net_id]);
  }
  const int remaining_test_nets = param_.test_iter_size() - test_net_id;
  if (has_net_param) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net_param";
      net_params[test_net_id].CopyFrom(param_.net_param());
    }
  }
  if (has_net_file) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net file: " + param_.net();
      ReadNetParamsFromTextFileOrDie(param_.net(), &net_params[test_net_id]);
    }
  }
  test_nets_.resize(num_test_net_instances);
  for (int i = 0; i < num_test_net_instances; ++i) {
    // Set the correct NetState.  We start with the solver defaults (lowest
    // precedence); then, merge in any NetState specified by the net_param
    // itself; finally, merge in any NetState specified by the test_state
    // (highest precedence).
    NetState net_state;
    net_state.set_phase(TEST);
    net_state.MergeFrom(net_params[i].state());
    if (param_.test_state_size()) {
      net_state.MergeFrom(param_.test_state(i));
    }
    net_params[i].mutable_state()->CopyFrom(net_state);
    LOG(INFO)
        << "Creating test net (#" << i << ") specified by " << sources[i];
    if (Caffe::root_solver()) {
      test_nets_[i].reset(new Net<Dtype>(net_params[i]));
    } else {
      test_nets_[i].reset(new Net<Dtype>(net_params[i],
          root_solver_->test_nets_[i].get()));
    }
    test_nets_[i]->set_debug_info(param_.debug_info());
  }
}

template <typename Dtype>
void Solver<Dtype>::Step(int iters) {
  const int start_iter = iter_;
  const int stop_iter = iter_ + iters;
  int average_loss = this->param_.average_loss();
  losses_.clear();
  smoothed_loss_ = 0;

  double eps = 0;
  while (iter_ < stop_iter) {
    // zero-init the params
    net_->ClearParamDiffs();
    if (param_.test_interval() && iter_ % param_.test_interval() == 0
        && (iter_ > 0 || param_.test_initialization())
        && Caffe::root_solver()) {
      // TestAll();
      if (requested_early_exit_) {
        // Break out of the while loop because stop was requested while testing.
        break;
      }
    }

    // Don't need; Sync has been down at DWBP
    // for (int i = 0; i < callbacks_.size(); ++i) {
    //   callbacks_[i]->on_start();
    // }
    const bool display = param_.display() && iter_ % param_.display() == 0;
    net_->set_debug_info(display && param_.debug_info());
    // accumulate the loss and gradient
    Dtype loss = 0;
    for (int i = 0; i < param_.iter_size(); ++i) {
      Timer tim = Timer();
      tim.Start();
      loss += ForwardBackwardWithDWBP();
      tim.Stop();
      eps += tim.Seconds();

      if (Caffe::root_solver() && iter_ % 20 == 19) {
        LOG(INFO) << "DWBP compute: " << eps;
        eps = 0;
      }
    }
    loss /= param_.iter_size();
    // average the loss across iterations for smoothed reporting
    UpdateSmoothedLoss(loss, start_iter, average_loss);
    if (display) {
      LOG_IF(INFO, Caffe::root_solver()) << "Iteration " << iter_
          << ", loss = " << smoothed_loss_;
      const vector<Blob<Dtype>*>& result = net_->output_blobs();
      int score_index = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        const string& output_name =
            net_->blob_names()[net_->output_blob_indices()[j]];
        const Dtype loss_weight =
            net_->blob_loss_weights()[net_->output_blob_indices()[j]];
        for (int k = 0; k < result[j]->count(); ++k) {
          ostringstream loss_msg_stream;
          if (loss_weight) {
            loss_msg_stream << " (* " << loss_weight
                            << " = " << loss_weight * result_vec[k] << " loss)";
          }
          LOG_IF(INFO, Caffe::root_solver()) << "    Train net output #"
              << score_index++ << ": " << output_name << " = "
              << result_vec[k] << loss_msg_stream.str();
        }
      }
    }
    // Increment the internal iter_ counter -- its value should always indicate
    // the number of times the weights have been updated.
    ++iter_;

    SolverAction::Enum request = GetRequestedAction();

    // Save a snapshot if needed.
    if ((param_.snapshot()
         && iter_ % param_.snapshot() == 0
         && Caffe::root_solver()) ||
         (request == SolverAction::SNAPSHOT)) {
      Snapshot();
    }
    if (SolverAction::STOP == request) {
      requested_early_exit_ = true;
      // Break out of training loop.
      break;
    }
  }
}

template <typename Dtype>
Dtype Solver<Dtype>::ForwardBackwardWithDWBP() {
  Timer tim = Timer();
  Dtype loss;
  tim.Start();
  net_->Forward(&loss);

  auto layers = net_->layers();
  vector<std::thread> threads;
  //auto& learnable_params = net_->learnable_params();
  for (int i = layers.size() - 1; i >= 0; --i) {
    // LOG(INFO) << "@@@ Layer " << layers[i]->type();

    net_->BackwardFromTo(i, i); // Backward layer i
    vector<int> learnable_params_id = layers[i]->learnable_params_id();
    if (learnable_params_id.empty())
      continue;

    // Do computation at main stream: diff = diff * lr
    // It appears that streams switch on GPU has large overhead
    ApplyUpdateParams(learnable_params_id);

    // A separate thread to sync grads/params IO
    for (int i : learnable_params_id) {
      threads.push_back(std::thread(&Solver<Dtype>::AsyncGradGPUs, this, i, false));
      // std::thread t(&Solver<Dtype>::AsyncGradGPUs, this, i, false);
      // t.join();
    }

  }
  // tim.Stop();

  // if (Caffe::root_solver()) 
  //   LOG(INFO) << "DWBP compute: " << tim.Seconds();

  //tim.Start();
  for (int i = 0; i < threads.size(); ++i)
    threads[i].join();
  tim.Stop();
  if (Caffe::root_solver()) 
    LOG(INFO) << "DWBP: " << tim.Seconds();

  static int mycount = 0;
  mycount++;
  if (mycount > 10) {
    send_[0]->Terminate();
    LOG(FATAL) << "-----------------------";
  }

  return loss;
}

template <typename Dtype>
Dtype mydiff(int N, Dtype* X, Dtype* Y) {
  std::shared_ptr<Dtype> buf(new Dtype[N]);
  caffe_copy<Dtype>(N, X, buf.get());             // buf = X
  caffe_axpy<Dtype>(N, Dtype(-1), Y, buf.get());     // buf = -Y + buf
  caffe_abs<Dtype>(N, buf.get(), buf.get());
  Dtype sum = 0;
  for (int i = 0; i < N; ++i)
    sum += buf.get()[i];

  return sum;
}

// DWBP: collect gradients for the finished layer, and sync new paramters for all GPUs
template <typename Dtype>
void Solver<Dtype>::AsyncGradGPUs(int id, bool is_init) {
  CUDA_CHECK(cudaSetDevice(this->param_.device_id()));
  int size = ps_buffer_[id]->count();
  CHECK(size > 0) << "Trying to sync with size = 0";

  // diff: GPU -> CPU -> PS
  int key = id;
  Dtype* ps_bf_diff = ps_buffer_[id]->mutable_cpu_diff();
  Dtype* gpu_diff;
  if (is_init) {
    gpu_diff = net_->learnable_params()[id]->mutable_gpu_data();
    cudaMemcpy(ps_bf_diff, gpu_diff, size*sizeof(Dtype), cudaMemcpyDeviceToHost);
    send_[key]->InitRun(ps_bf_diff, size*sizeof(Dtype));
  } else {
    gpu_diff = net_->learnable_params()[id]->mutable_gpu_diff();
    cudaMemcpy(ps_bf_diff, gpu_diff, size*sizeof(Dtype), cudaMemcpyDeviceToHost);
    // since it is add in ps, we need diff = -diff
    caffe_axpy<Dtype>(size, Dtype(-1), ps_bf_diff, ps_bf_diff);
    send_[key]->Run(ps_bf_diff, size*sizeof(Dtype));
  }

  // param: PS -> CPU -> GPU
  Dtype* ps_bf_data = ps_buffer_[id]->mutable_cpu_data();

  bool check = false;
  if (check) {
    // CHECK: ps_bf_data = ps_bf_data + ps_bf_diff 
    std::shared_ptr<Blob<Dtype> > ps_bf_true(new Blob<Dtype>(ps_buffer_[id]->shape()));
    caffe_add<Dtype>(size, ps_bf_data, ps_bf_diff, ps_bf_true->mutable_cpu_data());

    wait_->Run(ps_bf_data, size*sizeof(Dtype), key);
    // LOG(INFO) << "Wait " << key << ": " << ps_bf_data[0] 
    //   << " " << ps_bf_data[1] << " " << ps_bf_data[size-1];

    Dtype diff = mydiff(size, ps_bf_data, ps_bf_true->mutable_cpu_data());
    Dtype eps = 0.00001;
    CHECK(diff < eps && diff > -eps); 
    // LOG(INFO) << "Wait " << key << " with diff: " 
    //   << mydiff(size, ps_bf_data, ps_bf_true->mutable_cpu_data());
  } else {
    wait_->Run(ps_bf_data, size*sizeof(Dtype), key);
  }

  Dtype* gpu_data = net_->learnable_params()[id]->mutable_gpu_data();
  cudaMemcpy(gpu_data, ps_bf_data, size*sizeof(Dtype), cudaMemcpyHostToDevice);

  // CHECK(cudaMemcpy(dst, src, size*sizeof(Dtype), cudaMemcpyHostToHost)) << "---------";

  // // The root solver collects gradients
  // for (int i = 0; i < callbacks_.size(); ++i)
  //   callbacks_[i]->on_gradients_ready(size, offset, id[0]);

  // // Copy updated parameters to worker solvers 
  // for (int i = 0; i < callbacks_.size(); ++i)
  //   callbacks_[i]->on_start(size, offset, learnable_params_id[0]);
}



template <typename Dtype>
void Solver<Dtype>::Solve(const char* resume_file) {
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Solving " << net_->name();
  LOG(INFO) << "Learning Rate Policy: " << param_.lr_policy();

  // Initialize to false every time we start solving.
  requested_early_exit_ = false;

  if (resume_file) {
    LOG(INFO) << "Restoring previous solver status from " << resume_file;
    Restore(resume_file);
  }

  // For a network that is trained by the solver, no bottom or top vecs
  // should be given, and we will just provide dummy vecs.
  int start_iter = iter_;
  Step(param_.max_iter() - iter_);
  // If we haven't already, save a snapshot after optimization, unless
  // overridden by setting snapshot_after_train := false
  if (param_.snapshot_after_train()
      && (!param_.snapshot() || iter_ % param_.snapshot() != 0)) {
    Snapshot();
  }
  if (requested_early_exit_) {
    LOG(INFO) << "Optimization stopped early.";
    return;
  }
  // After the optimization is done, run an additional train and test pass to
  // display the train and test loss/outputs if appropriate (based on the
  // display and test_interval settings, respectively).  Unlike in the rest of
  // training, for the train net we only run a forward pass as we've already
  // updated the parameters "max_iter" times -- this final pass is only done to
  // display the loss, which is computed in the forward pass.
  if (param_.display() && iter_ % param_.display() == 0) {
    int average_loss = this->param_.average_loss();
    Dtype loss;
    net_->Forward(&loss);

    UpdateSmoothedLoss(loss, start_iter, average_loss);

    LOG(INFO) << "Iteration " << iter_ << ", loss = " << smoothed_loss_;
  }
  if (param_.test_interval() && iter_ % param_.test_interval() == 0) {
    TestAll();
  }
  LOG(INFO) << "Optimization Done.";
}

template <typename Dtype>
void Solver<Dtype>::TestAll() {
  for (int test_net_id = 0;
       test_net_id < test_nets_.size() && !requested_early_exit_;
       ++test_net_id) {
    Test(test_net_id);
  }
}

template <typename Dtype>
void Solver<Dtype>::Test(const int test_net_id) {
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Iteration " << iter_
            << ", Testing net (#" << test_net_id << ")";
  CHECK_NOTNULL(test_nets_[test_net_id].get())->
      ShareTrainedLayersWith(net_.get());
  vector<Dtype> test_score;
  vector<int> test_score_output_id;
  const shared_ptr<Net<Dtype> >& test_net = test_nets_[test_net_id];
  Dtype loss = 0;
  for (int i = 0; i < param_.test_iter(test_net_id); ++i) {
    SolverAction::Enum request = GetRequestedAction();
    // Check to see if stoppage of testing/training has been requested.
    while (request != SolverAction::NONE) {
        if (SolverAction::SNAPSHOT == request) {
          Snapshot();
        } else if (SolverAction::STOP == request) {
          requested_early_exit_ = true;
        }
        request = GetRequestedAction();
    }
    if (requested_early_exit_) {
      // break out of test loop.
      break;
    }

    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
        test_net->Forward(&iter_loss);
    if (param_.test_compute_loss()) {
      loss += iter_loss;
    }
    if (i == 0) {
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score.push_back(result_vec[k]);
          test_score_output_id.push_back(j);
        }
      }
    } else {
      int idx = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score[idx++] += result_vec[k];
        }
      }
    }
  }
  if (requested_early_exit_) {
    LOG(INFO)     << "Test interrupted.";
    return;
  }
  if (param_.test_compute_loss()) {
    loss /= param_.test_iter(test_net_id);
    LOG(INFO) << "Test loss: " << loss;
  }
  for (int i = 0; i < test_score.size(); ++i) {
    const int output_blob_index =
        test_net->output_blob_indices()[test_score_output_id[i]];
    const string& output_name = test_net->blob_names()[output_blob_index];
    const Dtype loss_weight = test_net->blob_loss_weights()[output_blob_index];
    ostringstream loss_msg_stream;
    const Dtype mean_score = test_score[i] / param_.test_iter(test_net_id);
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
              << mean_score << loss_msg_stream.str();
  }
}

template <typename Dtype>
void Solver<Dtype>::Snapshot() {
  CHECK(Caffe::root_solver());
  string model_filename;
  switch (param_.snapshot_format()) {
  case caffe::SolverParameter_SnapshotFormat_BINARYPROTO:
    model_filename = SnapshotToBinaryProto();
    break;
  case caffe::SolverParameter_SnapshotFormat_HDF5:
    model_filename = SnapshotToHDF5();
    break;
  default:
    LOG(FATAL) << "Unsupported snapshot format.";
  }

  SnapshotSolverState(model_filename);
}

template <typename Dtype>
void Solver<Dtype>::CheckSnapshotWritePermissions() {
  if (Caffe::root_solver() && param_.snapshot()) {
    CHECK(param_.has_snapshot_prefix())
        << "In solver params, snapshot is specified but snapshot_prefix is not";
    string probe_filename = SnapshotFilename(".tempfile");
    std::ofstream probe_ofs(probe_filename.c_str());
    if (probe_ofs.good()) {
      probe_ofs.close();
      std::remove(probe_filename.c_str());
    } else {
      LOG(FATAL) << "Cannot write to snapshot prefix '"
          << param_.snapshot_prefix() << "'.  Make sure "
          << "that the directory exists and is writeable.";
    }
  }
}

template <typename Dtype>
string Solver<Dtype>::SnapshotFilename(const string extension) {
  return param_.snapshot_prefix() + "_iter_" + caffe::format_int(iter_)
    + extension;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToBinaryProto() {
  string model_filename = SnapshotFilename(".caffemodel");
  LOG(INFO) << "Snapshotting to binary proto file " << model_filename;
  NetParameter net_param;
  net_->ToProto(&net_param, param_.snapshot_diff());
  WriteProtoToBinaryFile(net_param, model_filename);
  return model_filename;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToHDF5() {
  string model_filename = SnapshotFilename(".caffemodel.h5");
  LOG(INFO) << "Snapshotting to HDF5 file " << model_filename;
  net_->ToHDF5(model_filename, param_.snapshot_diff());
  return model_filename;
}

template <typename Dtype>
void Solver<Dtype>::Restore(const char* state_file) {
  CHECK(Caffe::root_solver());
  string state_filename(state_file);
  if (state_filename.size() >= 3 &&
      state_filename.compare(state_filename.size() - 3, 3, ".h5") == 0) {
    RestoreSolverStateFromHDF5(state_filename);
  } else {
    RestoreSolverStateFromBinaryProto(state_filename);
  }
}

template <typename Dtype>
void Solver<Dtype>::UpdateSmoothedLoss(Dtype loss, int start_iter,
    int average_loss) {
  if (losses_.size() < average_loss) {
    losses_.push_back(loss);
    int size = losses_.size();
    smoothed_loss_ = (smoothed_loss_ * (size - 1) + loss) / size;
  } else {
    int idx = (iter_ - start_iter) % average_loss;
    smoothed_loss_ += (loss - losses_[idx]) / average_loss;
    losses_[idx] = loss;
  }
}

INSTANTIATE_CLASS(Solver);

}  // namespace caffe
