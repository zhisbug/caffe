#ifndef ZMQ_COMMON_H
#define ZMQ_COMMON_H

#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <string>
#include <chrono>
#include <stdlib.h>
#include <time.h>

#include <glog/logging.h>

#include <zmq.hpp>
#include "caffe/common.hpp"
#include "caffe/proto/comm.pb.h"


namespace ps{

template <typename T>
class ThreadSafeHashTable{
public:
    void insert(int key, std::shared_ptr<T> new_value){
        std::lock_guard<std::mutex> lk(m_);
        table_[key] = new_value;
        cond_.notify_all();
    }
    std::shared_ptr<T> wait_and_get(int key){
        std::unique_lock<std::mutex> lk(m_);
        cond_.wait(lk, [this, key]{return table_.find(key)!=table_.end();});
        std::shared_ptr<T> ret = table_[key];
        table_.erase(key);
        cond_.notify_one(); // should not rely on spurious wake-up calls
        return ret;
    }
    static ThreadSafeHashTable<T> *get_table(){
        static ThreadSafeHashTable<T> table; 
        return &table;
    }
private:
    mutable std::mutex m_;
    std::condition_variable cond_;
    std::unordered_map<int, std::shared_ptr<T>> table_;
};

template <typename T>
class ThreadSafeQueue{
public:
    void push(T new_value){
        std::lock_guard<std::mutex> lk(m_);
        q_.push(new_value);
        cond_.notify_all();
    }

    T wait_and_pop(){
        std::unique_lock<std::mutex> lk(m_);
        cond_.wait(lk, [this]{return !q_.empty();});
        T ret = q_.front();
        q_.pop();
        //cond_.notify_one(); // should not rely on spurious wake-up calls
        return ret;
    }
private:
    mutable std::mutex m_;
    std::condition_variable cond_;
    std::queue<T> q_;
};

//zmq::context_t* get_context();
class ZMQContext{
public:
    static zmq::context_t& Get(){
        static zmq::context_t ctx(1);
        return ctx;
    }
};


class ZMQClient{
public:
    ZMQClient(const std::string &dst) : dst_(dst){ 
        socket_.reset(new zmq::socket_t(ZMQContext::Get(), ZMQ_DEALER));
        socket_->connect(dst);
    }
    ~ZMQClient(){
    }
    void Send(const Comm::Header &header, const void *buf = NULL){
        CHECK(socket_->send("", 0, ZMQ_SNDMORE) == 0);

        zmq::message_t request;
        std::string h_array;
        header.SerializeToString(&h_array);
        request.rebuild(h_array.c_str(), h_array.length());
        if (header.has_dh()){
            CHECK(buf);
            const char* c = h_array.c_str();
            LOG(INFO) << c[0] << c[1] << c[2] << c[3]; 
            CHECK(socket_->send(request, ZMQ_SNDMORE));
            float* f = (float*)buf;
            LOG(INFO) << f[0] << f[1] << f[2] << f[3];
            CHECK(socket_->send(buf, header.dh().length()) == header.dh().length());
        }else{
            CHECK(header.has_ch());
            CHECK(socket_->send(request, ZMQ_DONTWAIT));
            //CHECK(socket_->send(request, ZMQ_DONTWAIT) == h_array.length());
        }
    }
    void Recv(Comm::Header *header, void** buf = NULL){
        zmq::message_t envelope;
        CHECK(socket_->recv(&envelope));
        std::string es((char*)envelope.data(), envelope.size());
        LOG(INFO) << "envelope: " << envelope.size() << " " << es;
        CHECK(envelope.more());

        zmq::message_t h_m;
        socket_->recv(&h_m);
        CHECK(header->ParseFromArray(h_m.data(), h_m.size()));
        if (header->has_dh()){
            CHECK(h_m.more());
            if (*buf == NULL)
                *buf = new char[header->dh().length()];
            CHECK(socket_->recv(*buf, header->dh().length()) == header->dh().length());
        }else{
            CHECK(header->has_ch());
        }
    }

    // std::unordered_map<std::string, std::shared_ptr<zmq::socket_t>>& GetSockPool(){
    //      
    //     static std::unordered_map<std::string, std::shared_ptr<zmq::socket_t>> socket_pool;
    //     return socket_pool;
    // }
private:
    //static zmq::context_t context_;
    std::shared_ptr<zmq::socket_t> socket_;
    //static std::unordered_map<string, std::shared_ptr<zmq::socket_t>> socket_pool_;
    std::string dst_;
};

class ZMQServer{
public:
    ZMQServer(const std::string &src){ 
        socket_.reset(new zmq::socket_t(ZMQContext::Get(), ZMQ_ROUTER));
        // "If you're using ROUTER sockets, it's remarkably easy to lose messages
        // by accident, by sending malformed identity frames (or forgetting to send
        // an identity frame). In general setting the ZMQ_ROUTER_MANDATORY option
        // on ROUTER sockets is a good idea, but do also check the return code on
        // every send call." --- ZMQ Guide
        socket_->setsockopt(ZMQ_ROUTER_MANDATORY, 1);
        socket_->bind(src);
    }

    void Recv(std::string *id, Comm::Header *header, void** buf = NULL){
        zmq::message_t id_m;
        socket_->recv(&id_m);
        CHECK(id_m.more());
        *id = std::move(std::string((char*)id_m.data(), id_m.size()));

        zmq::message_t envelope;
        CHECK(socket_->recv(&envelope));
        CHECK(envelope.more());

        zmq::message_t h_m;
        socket_->recv(&h_m);
        CHECK(header->ParseFromArray(h_m.data(), h_m.size()));
        if (header->has_dh()){
            CHECK(h_m.more());
            if (*buf == NULL)
                *buf = new char[header->dh().length()];
            CHECK(socket_->recv(*buf, header->dh().length()) == header->dh().length());
        }else{
            CHECK(header->has_ch());
        }
    }

    void Send(const std::string &id, const Comm::Header &header, const void *buf = NULL){
        zmq::message_t id_m;
        id_m.rebuild(id.c_str(), id.length());
        socket_->send(id_m, ZMQ_SNDMORE);

        CHECK(socket_->send("", 0, ZMQ_SNDMORE) == 0);
        //zmq::message_t envelope("", 0);
        //CHECK(socket_->send(envelope, ZMQ_SNDMORE) == 0);

        zmq::message_t h_m;
        std::string h_array;
        header.SerializeToString(&h_array);
        h_m.rebuild(h_array.c_str(), h_array.length());
        if (header.has_dh()){
            //CHECK(socket_->send(h_m, ZMQ_SNDMORE) == h_array.length());
            CHECK(socket_->send(h_m, ZMQ_SNDMORE));
            CHECK(socket_->send(buf, header.dh().length()) == header.dh().length());
            // float* p = (float*) (buf);
            // LOG(INFO) << p[0] << " " << p[1] << " " << p[2];
        }else{
            CHECK(header.has_ch());
            CHECK(socket_->send(h_m));
            //CHECK(socket_->send(h_m) == h_array.length());
        }
    }
private:
    //static zmq::context_t context_;
    std::shared_ptr<zmq::socket_t> socket_;
};

}

#endif
