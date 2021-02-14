//
// tcpproxy_server.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2007 Arash Partow (http://www.partow.net)
// URL: http://www.partow.net/programming/tcpproxy/index.html
//
// Distributed under the Boost Software License, Version 1.0.
//
//
// Description
// ~~~~~~~~~~~
// The  objective of  the TCP  proxy server  is to  act  as  an
// intermediary  in order  to 'forward'  TCP based  connections
// from external clients onto a singular remote server.
//
// The communication flow in  the direction from the  client to
// the proxy to the server is called the upstream flow, and the
// communication flow in the  direction from the server  to the
// proxy  to  the  client   is  called  the  downstream   flow.
// Furthermore  the   up  and   down  stream   connections  are
// consolidated into a single concept known as a bridge.
//
// In the event  either the downstream  or upstream end  points
// disconnect, the proxy server will proceed to disconnect  the
// other  end  point  and  eventually  destroy  the  associated
// bridge.
//
// The following is a flow and structural diagram depicting the
// various elements  (proxy, server  and client)  and how  they
// connect and interact with each other.

//
//                                    ---> upstream --->           +---------------+
//                                                     +---->------>               |
//                               +-----------+         |           | Remote Server |
//                     +--------->          [x]--->----+  +---<---[x]              |
//                     |         | TCP Proxy |            |        +---------------+
// +-----------+       |  +--<--[x] Server   <-----<------+
// |          [x]--->--+  |      +-----------+
// |  Client   |          |
// |           <-----<----+
// +-----------+
//                <--- downstream <---
//
//


#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/algorithm/string.hpp>

namespace tcp_proxy
{
   namespace ip = boost::asio::ip;

   class bridge : public boost::enable_shared_from_this<bridge>
   {
   public:

      typedef ip::tcp::socket socket_type;
      typedef boost::shared_ptr<bridge> ptr_type;

      bridge(boost::asio::io_service& ios)
      : downstream_socket_(ios),
        upstream_socket_  (ios)
      {
         //std::clog << "create brdige " << (count ++) << std::endl;
      }
      ~bridge()
      {
         //std::clog << "destroy brdige " << (--count) << std::endl;
      }

      socket_type& downstream_socket()
      {
         // Client socket
         return downstream_socket_;
      }

      socket_type& upstream_socket()
      {
         // Remote server socket
         return upstream_socket_;
      }
      void read_request()
      {
         //std::clog << "read_request" << std::endl;
         downstream_socket_.async_read_some(boost::asio::buffer(downstream_data_, max_data_length), boost::bind(&bridge::handle_read, shared_from_this(), 
         boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      }

      void handle_read(const boost::system::error_code& error, std::size_t bytes_transferred)
      {
         //std::clog << "handle_read" << std::endl;
         if (!error && bytes_transferred > 0)
         {
            static const std::string header_end = "\r\n\r\n";
            int old_size = received_data_.size();
            received_data_.resize(received_data_.size() + bytes_transferred);
            std::copy(downstream_data_, downstream_data_ + bytes_transferred, received_data_.begin() + old_size);
            //std::clog << "read_request\n" << received_data_ << std::endl;
            auto index = received_data_.find(header_end);
            if (index != std::string::npos)
            {
               auto host_port = parse_header(received_data_);
               //std::clog << host_port.first << ":" << host_port.second << std::endl;
               start(host_port.first, host_port.second);
            }
            else
            {
               read_request();
            }
         }
         else
         {
            close();
         }
      }

      std::pair<const std::string, const std::string> parse_header(const std::string& header) const
      {
      // CONNECT www.reddit.com:443 HTTP/1.1
      // Host: www.reddit.com:443
      // Proxy-Connection: keep-alive
      // User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/88.0.4324.150 Safari/537.36
         std::vector<std::string> headers;
         boost::split(headers, header, boost::is_any_of("\r\n"), boost::token_compress_on);
         /*for (auto it : headers)
         {
            std::clog << it << "\n";
         }
         std::clog << std::endl;*/
         std::string host, port;
         if (headers.size() > 0)
         {
            std::vector<std::string> splitvec;
            boost::split(splitvec, headers[0], boost::is_any_of(" "), boost::token_compress_on);
            if (splitvec.size() >= 3)
            {
               std::vector<std::string> splitvec2;
               boost::split(splitvec2, splitvec[1], boost::is_any_of(":"), boost::token_compress_on);
               if (splitvec2.size() == 2) 
               {
                  //std::clog << splitvec2[0] << " " << splitvec2[1] << std::endl;
                  host = splitvec2[0];
                  port = splitvec2[1];  
               }
            }
         }
         return std::make_pair(host, port);
      }

      void response(unsigned status, const std::string& message)
      {
         std::string reply = std::string("HTTP/1.0 " ) + std::to_string(status) + " " + message + "\r\n";
         reply += "Proxy-agent: proxy\r\n\r\n";
         //std::clog << "response from server\n" << reply << std::endl;
         downstream_socket_.async_send(boost::asio::buffer(reply, reply.size()), boost::bind(&bridge::handle_send, shared_from_this(), 
         boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      }

      void handle_send(const boost::system::error_code& error, std::size_t bytes_transferred)
      {
         if (error)
         {
            close();
         }
      }

      void start(const std::string& upstream_host, const std::string& upstream_port)
      {
         /*{
            boost::mutex::scoped_lock lock(mutex_);
            std::clog << "start " << upstream_host << " " << upstream_port << " " << (count++) << std::endl;
         }*/
         // Attempt connection to remote server (upstream side)
         boost::asio::io_service io_service;
         boost::asio::ip::tcp::resolver resolver(io_service);
         boost::asio::ip::tcp::resolver::query query(upstream_host, upstream_port);
         boost::asio::ip::tcp::resolver::iterator iter = resolver.resolve(query);
         upstream_socket_.async_connect(
               iter->endpoint(),
               boost::bind(&bridge::handle_upstream_connect,
                    shared_from_this(),
                    boost::asio::placeholders::error));

         // upstream_socket_.async_connect(
         //      ip::tcp::endpoint(
         //           boost::asio::ip::address::from_string(upstream_host),
         //           upstream_port),
         //       boost::bind(&bridge::handle_upstream_connect,
         //            shared_from_this(),
         //            boost::asio::placeholders::error));
         //std::clog << "start" << std::endl;
      }

      void handle_upstream_connect(const boost::system::error_code& error)
      {
         //response(unsigned status, const std::string& message);
         //std::clog << "handle_upstream_connect" << std::endl;
         if (!error)
         {
            response(200, "Connection established");
            // Setup async read from remote server (upstream)
            upstream_socket_.async_read_some(
                 boost::asio::buffer(upstream_data_,max_data_length),
                 boost::bind(&bridge::handle_upstream_read,
                      shared_from_this(),
                      boost::asio::placeholders::error,
                      boost::asio::placeholders::bytes_transferred));

            // Setup async read from client (downstream)
            downstream_socket_.async_read_some(
                 boost::asio::buffer(downstream_data_,max_data_length),
                 boost::bind(&bridge::handle_downstream_read,
                      shared_from_this(),
                      boost::asio::placeholders::error,
                      boost::asio::placeholders::bytes_transferred));
         }
         else
         {
            response(404, "Connection failed");
            close();
            //std::clog << "closed handle_upstream_connect" << std::endl;
         }
      }

   private:

      /*
         Section A: Remote Server --> Proxy --> Client
         Process data recieved from remote sever then send to client.
      */

      // Read from remote server complete, now send data to client
      void handle_upstream_read(const boost::system::error_code& error,
                                const size_t& bytes_transferred)
      {
         if (!error)
         {
            async_write(downstream_socket_,
                 boost::asio::buffer(upstream_data_,bytes_transferred),
                 boost::bind(&bridge::handle_downstream_write,
                      shared_from_this(),
                      boost::asio::placeholders::error));
         }
         else
         {
            close();
            //std::clog << "closed handle_upstream_read" << std::endl;
         }
      }

      // Write to client complete, Async read from remote server
      void handle_downstream_write(const boost::system::error_code& error)
      {
         if (!error)
         {
            upstream_socket_.async_read_some(
                 boost::asio::buffer(upstream_data_,max_data_length),
                 boost::bind(&bridge::handle_upstream_read,
                      shared_from_this(),
                      boost::asio::placeholders::error,
                      boost::asio::placeholders::bytes_transferred));
         }
         else
         {
            close();
            //std::clog << "closed" << std::endl;
         }
      }
      // *** End Of Section A ***


      /*
         Section B: Client --> Proxy --> Remove Server
         Process data recieved from client then write to remove server.
      */

      // Read from client complete, now send data to remote server
      void handle_downstream_read(const boost::system::error_code& error,
                                  const size_t& bytes_transferred)
      {
         if (!error)
         {
            async_write(upstream_socket_,
                  boost::asio::buffer(downstream_data_,bytes_transferred),
                  boost::bind(&bridge::handle_upstream_write,
                        shared_from_this(),
                        boost::asio::placeholders::error));
         }
         else
         {
            close();
            //std::clog << "closed handle_downstream_read" << std::endl;
         }
      }

      // Write to remote server complete, Async read from client
      void handle_upstream_write(const boost::system::error_code& error)
      {
         if (!error)
         {
            downstream_socket_.async_read_some(
                 boost::asio::buffer(downstream_data_,max_data_length),
                 boost::bind(&bridge::handle_downstream_read,
                      shared_from_this(),
                      boost::asio::placeholders::error,
                      boost::asio::placeholders::bytes_transferred));
         }
         else
         {
            close();
            //std::clog << "closed" << std::endl;
         }
      }
      // *** End Of Section B ***

      void close()
      {
         boost::mutex::scoped_lock lock(mutex_);
         // count--;
         // std::clog << "close " << count << std::endl;
         if (downstream_socket_.is_open())
         {
            downstream_socket_.close();
         }

         if (upstream_socket_.is_open())
         {
            upstream_socket_.close();
         }
      }

      socket_type downstream_socket_;
      socket_type upstream_socket_;

      enum { max_data_length = 8192 }; //8KB
      unsigned char downstream_data_[max_data_length];
      unsigned char upstream_data_  [max_data_length];
      std::string received_data_;

      boost::mutex mutex_;


   public:

      class acceptor
      {
      public:

         acceptor(boost::asio::io_service& io_service,
                  const std::string& local_host, unsigned short local_port)
         : io_service_(io_service),
           localhost_address(boost::asio::ip::address_v4::from_string(local_host)),
           acceptor_(io_service_,ip::tcp::endpoint(localhost_address,local_port))
         {}

         bool accept_connections()
         {
            try
            {
               session_ = boost::shared_ptr<bridge>(new bridge(io_service_));

               acceptor_.async_accept(session_->downstream_socket(),
                    boost::bind(&acceptor::handle_accept,
                         this,
                         boost::asio::placeholders::error));
            }
            catch(std::exception& e)
            {
               std::cerr << "acceptor exception: " << e.what() << std::endl;
               return false;
            }

            return true;
         }

      private:

         void handle_accept(const boost::system::error_code& error)
         {
            if (!error)
            {
               //session_->start(upstream_host_,upstream_port_);
               session_->read_request();

               if (!accept_connections())
               {
                  std::cerr << "Failure during call to accept." << std::endl;
               }
            }
            else
            {
               std::cerr << "Error: " << error.message() << std::endl;
            }
         }

         boost::asio::io_service& io_service_;
         ip::address_v4 localhost_address;
         ip::tcp::acceptor acceptor_;
         ptr_type session_;
      };

   };
}

int main(int argc, char* argv[])
{
   if (argc != 3)
   {
      std::cerr << "usage: tcpproxy_server <local host ip> <local port>" << std::endl;
      return 1;
   }

   const unsigned short local_port   = static_cast<unsigned short>(::atoi(argv[2]));
   //const unsigned short forward_port = static_cast<unsigned short>(::atoi(argv[4]));
   const std::string local_host      = argv[1];
   //const std::string forward_host    = argv[3];

   boost::asio::io_service ios;

   try
   {
      tcp_proxy::bridge::acceptor acceptor(ios,
                                           local_host, local_port);

      acceptor.accept_connections();

      ios.run();
   }
   catch(std::exception& e)
   {
      std::cerr << "Error: " << e.what() << std::endl;
      return 1;
   }

   return 0;
}

/*
 * [Note] On posix systems the tcp proxy server build command is as follows:
 * c++ -pedantic -ansi -Wall -Werror -O3 -o tcpproxy_server tcpproxy_server.cpp -L/usr/lib -lstdc++ -lpthread -lboost_thread -lboost_system
 */
