#ifndef ZMQ_MASTER_H
#define ZMQ_MASTER_H

#include <thread>
#include "zmq_common.hpp"

namespace ps{

class PSMaster{
public:
    PSMaster(std::string my_addr, int expected_servers) : 
        my_addr_(my_addr), servers_(0){
        LOG(INFO) << "Master Addr: \"" << my_addr << "\"";
        LOG(INFO) << "Expected Servers: " << expected_servers;
        master_ = std::shared_ptr<ZMQServer>(new ZMQServer(my_addr_));
        dealer_ = std::shared_ptr<std::thread>(new std::thread([this, expected_servers]
            {
                LOG(INFO) << "Server Registering...";
                while(1){
                    Comm::Header request;
                    std::string id;
                    master_->Recv(&id, &request);
                    CHECK(request.has_ch());
                    const Comm::CtrlHeader *ch = &request.ch();
                    if (ch->role() == Comm::CtrlHeader::SERVER){//server
                        LOG(INFO) << "Received Server Register";
                        if (ch->op() == Comm::CtrlHeader::ADD){//add
                            CHECK(ch->addr_size() == 1);
                            LOG(INFO) << "Server[" << servers_ << "]("
                                           << ch->addr(0) 
                                           << ") registered successfully";
                            if (++servers_ == expected_servers){
                                LOG(INFO) << "All expected servers registered";
                            }
                            if (to_servers_.find(ch->addr(0)) == to_servers_.end()){
                                to_servers_[ch->addr(0)] = std::shared_ptr<ZMQClient>(
                                        new ZMQClient(ch->addr(0)));
                                servers_addr_.push_back(ch->addr(0));
                            }
                        }
                    }else{//worker
                        if (ch->op() == Comm::CtrlHeader::ADD){//for listener
                            LOG(INFO) << "Received Listener Register";
                            CHECK(ch->addr_size() == 1);
                            CHECK(to_servers_.size() == expected_servers);    
                            Comm::Header reply;
                            Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                            reply_ch->set_role(Comm::CtrlHeader::SERVER);
                            reply_ch->set_op(Comm::CtrlHeader::ADD);
                            reply_ch->add_addr(ch->addr(0));
                            //for (int i = 0; i < to_servers_.size(); i++){
                                //LOG(INFO) << "Sending Listener info to "
                                               //<< servers_addr_[i];
                                //to_servers_[i]->Send(reply);
                                //to_servers_[i]->Recv(&reply);
                            //}
                            for (const auto &i : to_servers_){
                                LOG(INFO) << "Sending Listener info to " << i.first;
                                i.second->Send(reply);
                                i.second->Recv(&reply);
                            }
                            reply_ch->set_role(Comm::CtrlHeader::WORKER);
                            reply_ch->set_op(Comm::CtrlHeader::ADD);
                            master_->Send(id, reply);//respond to listener
                            if (to_workers_.find(ch->addr(0)) == to_workers_.end()){
                                to_workers_[ch->addr(0)] = std::shared_ptr<ZMQClient>(
                                        new ZMQClient(ch->addr(0)));
                            }
                            //workers_addr_.push_back(ch->addr(0));
                        }else if (ch->op() == Comm::CtrlHeader::TERMINATE){
                            Comm::Header reply;
                            Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                            reply_ch->set_role(Comm::CtrlHeader::SERVER);
                            reply_ch->set_op(Comm::CtrlHeader::TERMINATE);
                            //for (int i = 0; i < to_servers_.size(); i++){
                                //LOG(INFO) << "Sending terminate message to "
                                               //<< servers_addr_[i];
                                //to_servers_[i]->Send(reply);
                            //}
                            for (const auto &i : to_servers_){
                                LOG(INFO) << "Sending terminate message to " << i.first;
                                i.second->Send(reply); 
                            }
                            servers_addr_.clear();
                            to_servers_.clear();
                            //for (int i = 0; i < to_workers_.size(); i++){
                                //Comm::Header reply;
                                //Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                                //reply_ch->set_role(Comm::CtrlHeader::WORKER);
                                //reply_ch->set_op(Comm::CtrlHeader::TERMINATE);
                                //LOG(INFO) << "Sending terminate message to "
                                               //<< workers_addr_[i];
                                //to_workers_[i]->Send(reply);
                            //}
                            reply_ch->set_role(Comm::CtrlHeader::WORKER);
                            for (const auto &i : to_workers_){
                                LOG(INFO) << "Sending terminate message to " << i.first;
                                i.second->Send(reply); 
                            }
                            //workers_addr_.clear();
                            to_workers_.clear();
                            servers_ = 0;
                        }else if (ch->op() == Comm::CtrlHeader::QUERY_SERVER){
                            //for worker to server communication
                            LOG(INFO) << "Received Sender Query the Addr of Server for Certain Key";
                            Comm::Header reply;
                            Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                            reply_ch->set_role(Comm::CtrlHeader::WORKER);
                            reply_ch->set_op(Comm::CtrlHeader::QUERY_SERVER);
                            CHECK(ch->has_key());
                            reply_ch->add_addr(servers_addr_[ch->key()%servers_addr_.size()]);
                            master_->Send(id, reply);//respond to sender
                        }else if (ch->op() == Comm::CtrlHeader::QUERY_WORKERS){
                            //for worker to worker communication
                            LOG(INFO) << "Received Sender Query the Addr of Other Workers";
                            Comm::Header reply;
                            Comm::CtrlHeader *reply_ch = reply.mutable_ch();
                            reply_ch->set_role(Comm::CtrlHeader::WORKER);
                            reply_ch->set_op(Comm::CtrlHeader::QUERY_WORKERS);
                            //for (int i = 0; i < workers_addr_.size(); i++){
                                //reply_ch->add_addr(workers_addr_[i]);
                            //}
                            for (const auto &i : to_workers_){
                                reply_ch->add_addr(i.first);
                            }
                            master_->Send(id, reply);//respond to sender
                        }
                    }
                    LOG(INFO) << "One Request issued";
                }
            }), [](std::thread *t){t->join();});
    }
    void Run(){
        dealer_->join(); 
    }
    
private:
    std::shared_ptr<ZMQServer> master_;
    std::shared_ptr<std::thread> dealer_;
    std::vector<std::string> servers_addr_;
    std::unordered_map<std::string, std::shared_ptr<ZMQClient>> to_servers_;
    //std::vector<std::string> workers_addr_;
    //std::vector<std::shared_ptr<ZMQClient>> to_workers_;
    std::unordered_map<std::string, std::shared_ptr<ZMQClient>> to_workers_;
    int servers_;
    std::string my_addr_;
};

}

#endif
