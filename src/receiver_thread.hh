// Standard headers
#include <condition_variable>
#include <mutex>
#include <string>
#include <map>

struct ForeignScanState;

namespace zmq
{
    class context_t;
    class socket_t;
}

namespace virtdb
{
    class query;
    class data_handler;
}

namespace virtdb {
    class receiver_thread
    {
        private:
            struct cv_data
            {
                std::mutex mutex;
                std::condition_variable variable;
            };

            bool done = false;
            std::map<std::string, cv_data> cv;
            std::map<const ForeignScanState* const, data_handler* > active_queries;
            zmq::context_t* zmq_context;
            zmq::socket_t*  data_socket;
            std::string     query_address = "";
            std::string     data_address = "";
            cv_data         query_url_cv;
            cv_data         data_url_cv;

            void add_query(const ForeignScanState* const node, const virtdb::query& query);

        public:
            receiver_thread(zmq::context_t* context);
            virtual ~receiver_thread();

            void remove_query(const ForeignScanState* const node);
            void stop();
            data_handler* get_data_handler(const ForeignScanState* const node);
            data_handler* get_data_handler(std::string queryid);
            bool wait_for_data(const ForeignScanState* const node);
            void run();

            void send_query(const ForeignScanState* const node, const virtdb::query& query);
            void set_query_url(const std::string& url);
            void set_data_url(const std::string& url);


    };
}
