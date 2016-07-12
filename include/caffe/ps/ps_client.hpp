#ifndef PS_CLIENT_H
#define PS_CLIENT_H

#include <vector>
#include <thread>
#include "caffe/ps/zmq_common.hpp"

#include "caffe/blob.hpp"
#include "caffe/layers/inner_product_layer.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/benchmark.hpp"

namespace ps{

using std::string;
using std::shared_ptr;
using std::thread;
using std::vector;

template <typename T>
class Worker{
public:
    Worker(int key, int bytes, T* data, T* diff, string master_addr) :
        data_(data), diff_(diff)
    {
        static int id = 0;
        id_ = id++;

        // TODO: key is not used here
        LOG(INFO) << "id: " << id_ << " key: " << key << "\tbytes: " << bytes;
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_key(id_);
        dh->set_type(DataHeaderTypeWrapper<T>());
        dh->set_length(bytes);

        to_master_.reset(new ZMQClient(master_addr));

        // wait for all servers to register
        while(true){
            Comm::Header h;
            h.mutable_ch()->set_role(Comm::CtrlHeader::WORKER);
            h.mutable_ch()->set_op(Comm::CtrlHeader::ASK_INIT);
            to_master_->Send(h);
	    to_master_->Recv(&h);
	    if(h.ch().op() == Comm::CtrlHeader::GO){
	        break;
	    }else if(h.ch().op() == Comm::CtrlHeader::WAIT){
	        LOG(INFO) << id_ << " waiting for all servers to register...";
	        sleep(1);
	    }else{
	        LOG(FATAL) << "Op not recognized.";
	    }
        }

        LOG(INFO) << "Register at master " << master_addr;
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::QUERY_SERVER);
        ctrl4master->set_key(dh->key());
        to_master_->Send(header4master);
        to_master_->Recv(&header4master);
        CHECK(header4master.ch().addr_size() == 1);

        LOG(INFO) << "Register at server " << header4master.ch().addr(0);
        client_.reset(new ZMQClient(header4master.ch().addr(0))); 
        Comm::Header header4server;
        Comm::CtrlHeader *ctrl4server = header4server.mutable_ch();
        ctrl4server->set_role(Comm::CtrlHeader::WORKER);
        ctrl4server->set_op(Comm::CtrlHeader::ADD);
        ctrl4server->set_key(dh->key());
        client_->Send(header4server);
    }

    virtual ~Worker() {}

    void InitWithPS(int client_id){
        inited_ = true;
        client_id_ = client_id;
        Comm::DataHeader *dh = header_.mutable_dh();

        if(client_id == 0){
            // check if all clients have registered
            while(true){
                Comm::Header h;
                h.mutable_ch()->set_key(id_);
                h.mutable_ch()->set_role(Comm::CtrlHeader::WORKER);
                h.mutable_ch()->set_op(Comm::CtrlHeader::ASK_INIT);
                client_->Send(h);
		client_->Recv(&h);
		if(h.ch().op() == Comm::CtrlHeader::GO){
		    LOG(INFO) << "GO";
		    break;
		}else if(h.ch().op() == Comm::CtrlHeader::WAIT){
		    LOG(INFO) << id_ << " waiting for all clients to register...";
		    sleep(1);
		}else{
		    LOG(FATAL) << "Op not recognized.";
		}
            }
            
            dh->set_iter(0);
            dh->set_is_init(true);
            client_->Send(header_, data_);
            dh->set_is_init(false);
        }

        Comm::Header h;
        client_->Recv(&h, (void**)&data_);
        dh->set_iter(h.dh().iter());
        dh->set_num_worker(h.dh().num_worker());
    }

    virtual void Push(){
        CHECK(inited_);
        client_->Send(header_, diff_);
    }

    inline void IncIter() {
        CHECK(inited_);
        Comm::DataHeader *dh = header_.mutable_dh();
        dh->set_iter(header_.dh().iter()+1);
    }

    virtual void Pull(){
        CHECK(inited_);
        Comm::Header h;
        client_->Recv(&h, (void**)&data_);
        CHECK(h.dh().iter() == header_.dh().iter());
    }
    
    void Reset(int client_id){
        if(client_id == 0){
            Comm::Header h;
            h.mutable_ch()->set_key(id_);
            h.mutable_ch()->set_role(Comm::CtrlHeader::WORKER);
            h.mutable_ch()->set_op(Comm::CtrlHeader::RESET);
            client_->Send(h);
	}
        Comm::Header h;
        client_->Recv(&h);
    }

    inline int iter() { return header_.dh().iter(); }
    inline int num_worker() { return header_.dh().num_worker(); }
    inline int bytes() { return header_.dh().length(); }

    inline T* data() { return data_; }
    inline T* diff() { return diff_; }

    void Terminate(){
        Comm::Header header4master;
        Comm::CtrlHeader *ctrl4master = header4master.mutable_ch();
        ctrl4master->set_role(Comm::CtrlHeader::WORKER);
        ctrl4master->set_op(Comm::CtrlHeader::TERMINATE);
        to_master_->Send(header4master);
    }

protected:
    Comm::Header header_;
    shared_ptr<ZMQClient> to_master_;
    shared_ptr<ZMQClient> client_;
    T* data_;
    T* diff_;
    int id_ = -1;
    int client_id_ = -1;
    bool inited_ = false;

    template <typename Type>
    Comm::DataHeader::DataType DataHeaderTypeWrapper(){
        if (std::is_same<Type, float>:: value){
            return Comm::DataHeader::FLOAT; 
        }else if (std::is_same<Type, double>::value){
            return Comm::DataHeader::DOUBLE;
        }else if (std::is_same<Type, int>::value){
            return Comm::DataHeader::INT32; 
        }else if (std::is_same<Type, char>::value){
            return Comm::DataHeader::BYTE; 
        }
    }
};


template <typename T>
class SVBWorker : public Worker<T>{
public:
    SVBWorker(int key, int bytes, T* data, T* diff, string master_addr,
            caffe::Blob<T>* top, caffe::Blob<T>* bottom,
            caffe::InnerProductLayer<T>* layer)
        : Worker<T>(key, bytes, data, diff, master_addr)
    {
        top_ = top;
        bottom_ = bottom;
        layer_ = layer;
        M_ = layer_->M();
        K_ = layer_->K();
        N_ = layer_->N();
        transpose_ = layer_->transpose();

        svb_bytes_total_ += top_->count()*sizeof(T);
        svb_bytes_total_ += bottom_->count()*sizeof(T);
        this->header_.mutable_dh()->add_svb_length(svb_bytes_total_);

        svb_buf_ = (T*)new char[svb_bytes_total_];

        LOG(INFO) << "top " << top_->shape_string();
        LOG(INFO) << "bottom " << bottom_->shape_string();
        LOG(INFO) << "svb_bytes_total_ " << svb_bytes_total_;
    }

    ~SVBWorker() { delete[] svb_buf_; }

    void Push() override{
        CHECK(this->inited_);
        Comm::Header h = this->header_;
        h.mutable_dh()->set_length(svb_bytes_total_);

        // LOG(INFO) << "cudaMemcpy top_diff...";
        cudaMemcpy(svb_buf_,
                   top_->mutable_gpu_diff(),
                   top_->count()*sizeof(T), cudaMemcpyDeviceToHost);
        // LOG(INFO) << "cudaMemcpy bottom_data...";
        cudaMemcpy(svb_buf_ + top_->count(),
                   bottom_->mutable_gpu_data(),
                   bottom_->count()*sizeof(T), cudaMemcpyDeviceToHost);

        // LOG(INFO) << "Send svb_buf...";
        this->client_->Send(h, svb_buf_);
        // LOG(INFO) << svb_buf_[0] << " " << svb_buf_[1] << " "
        //     << svb_buf_[top_->count()] << " " << svb_buf_[top_->count()+1];
    }

    void Pull() override{
        CHECK(this->inited_);
        for (int i = 0; i < this->num_worker(); ++i){

            caffe::Timer tim = caffe::Timer();
            tim.Start();

            Comm::Header h;
            this->client_->Recv(&h, (void**)&svb_buf_);
            CHECK(h.dh().iter() == this->header_.mutable_dh()->iter());
            CHECK(h.dh().length() == svb_bytes_total_);

            // LOG(INFO) << svb_buf_[0] << " " << svb_buf_[1] << " "
            //     << svb_buf_[top_->count()] << " " << svb_buf_[top_->count()+1];

            tim.Stop();
            LOG_IF(INFO, this->id_ == 10) << "Pull Buffer: " << tim.Seconds();

            tim.Start();

            // TODO: MATRIX MULTIPLICATION
            T* top_diff = svb_buf_;
            T* bottom_data = svb_buf_ + top_->count();
            if (transpose_) {
                // caffe::caffe_cpu_gemm<T>(CblasTrans, CblasNoTrans,
                //     K_, N_, M_,
                //     (T)-1., bottom_data, top_diff,
                //     (T)1., this->data());
                caffe::caffe_cpu_gemm<T>(CblasTrans, CblasNoTrans,
                    K_, N_, M_,
                    (T)-1., bottom_data, top_diff,
                    (T)0., this->diff());
            } else {
                // caffe::caffe_cpu_gemm<T>(CblasTrans, CblasNoTrans,
                //     N_, K_, M_,
                //     (T)-1., top_diff, bottom_data,
                //     (T)1., this->data());
                caffe::caffe_cpu_gemm<T>(CblasTrans, CblasNoTrans,
                    N_, K_, M_,
                    (T)-1., top_diff, bottom_data,
                    (T)0., this->diff());
            }

            tim.Stop();
            LOG_IF(INFO, this->id_ == 10) << "Compute Buffer: " << tim.Seconds();
        }
    }

    T* svb_buf() { return svb_buf_; }

private:
    vector<int> svb_bytes_;
    int svb_bytes_total_ = 0;
    T* svb_buf_;
    caffe::Blob<T>* top_;
    caffe::Blob<T>* bottom_;

    caffe::InnerProductLayer<T>* layer_;
    int M_, K_, N_;
    bool transpose_;  ///< if true, assume transposed weights
};


template <typename T>
class WorkerGroup{
public:
    WorkerGroup(int key, int bytes, T* data, T* diff, string master_addr)
    {
        const int batch_size = 512 * 1024; // 512 KB
	const int shift_num = batch_size / sizeof(T);
	
	for(int i = 0; i*batch_size < bytes; ++i){
	    int bytes_left = bytes - i*batch_size;
	    int my_batch_size = bytes_left < batch_size ? bytes_left : batch_size;
	    T*  my_data = data + i*shift_num;
	    T*  my_diff = diff + i*shift_num;
	    
	    worker_.push_back(shared_ptr<Worker<T> >(
	        new Worker<T>(key, my_batch_size, my_data, my_diff, master_addr)
	        ));
	}
    }

    ~WorkerGroup() {}

    void InitWithPS(int client_id){
        for(auto& w : worker_)
	    w->InitWithPS(client_id);
    }

    void Push(){
        //for(auto& w : worker_)
	//    w->Push();
        vector<thread> threads;
	for(int i = 0; i < worker_.size(); ++i)
	    threads.push_back(thread([this, i] { worker_[i]->Push(); }));
	for(auto& t : threads)
	    t.join();
    }

    inline void IncIter(){
        for(auto& w : worker_)
	    w->IncIter();
    }

    virtual void Pull(){
        vector<thread> threads;
	for(int i = 0; i < worker_.size(); ++i)
	    threads.push_back(thread([this, i] { worker_[i]->Pull(); }));
	for(auto& t : threads)
	    t.join();
    }

    inline int iter() { return worker_[0]->iter(); }
    inline int num_worker() { return worker_[0]->num_worker(); }
    inline int bytes() { return worker_[0]->bytes(); }

    inline T* data() { return worker_[0]->data(); }
    inline T* diff() { return worker_[0]->diff(); }

    void Terminate() { worker_[0]->Terminate(); }

protected:
    vector<shared_ptr<Worker<T> > > worker_;
};

}

#endif
