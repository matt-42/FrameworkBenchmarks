#include "lithium_http_backend.hh"

#if TFB_MYSQL
  #include "lithium_mysql.hh"
#elif TFB_PGSQL
  #include "lithium_pgsql.hh"
#endif

#include "symbols.hh"
using namespace li;

template <typename B>
void escape_html_entities(B& buffer, const std::string& data)
{
    for(size_t pos = 0; pos != data.size(); ++pos) {
        switch(data[pos]) {
            case '&':  buffer << "&amp;";       break;
            case '\"': buffer << "&quot;";      break;
            case '\'': buffer << "&apos;";      break;
            case '<':  buffer << "&lt;";        break;
            case '>':  buffer << "&gt;";        break;
            default:   buffer << data[pos]; break;
        }
    }
}

int main(int argc, char* argv[]) {

  if (argc != 4)
  {
    std::cerr << "Usage: " << argv[0] << " sql_host port nprocs" << std::endl;
    return 1;
  }

  int port = atoi(argv[2]);
  int nprocs = atoi(argv[3]);

#if TFB_MYSQL
  auto sql_db =
    mysql_database(s::host = argv[1], s::database = "hello_world", s::user = "benchmarkdbuser",
                   s::password = "benchmarkdbpass", s::port = 3306, s::charset = "utf8");
  int mysql_max_connection = sql_db.connect()("SELECT @@GLOBAL.max_connections;").read<int>() * 0.75;
  li::max_mysql_connections_per_thread = (mysql_max_connection / nprocs);
  std::cout << "Using " << li::max_mysql_connections_per_thread << " connections per thread. " << nprocs << " threads." << std::endl; 

#elif TFB_PGSQL
  auto sql_db =
    pgsql_database(s::host = argv[1], s::database = "hello_world", s::user = "benchmarkdbuser",
                   s::password = "benchmarkdbpass", s::port = 5432, s::charset = "utf8");
  int pgsql_max_connection = atoi(sql_db.connect()("SHOW max_connections;").read<std::string>().c_str()) * 0.75;
  li::max_pgsql_connections_per_thread = (pgsql_max_connection / nprocs);
  std::cout << "Using " << li::max_pgsql_connections_per_thread << " connections per thread. " << nprocs << " threads." << std::endl; 
#endif

  auto fortunes = sql_orm_schema(sql_db, "Fortune").fields(
    s::id(s::auto_increment, s::primary_key) = int(),
    s::message = std::string());

  auto random_numbers = sql_orm_schema(sql_db, "World").fields(
    s::id(s::auto_increment, s::primary_key) = int(),
    s::randomNumber = int());

  http_api my_api;

  my_api.get("/plaintext") = [&](http_request& request, http_response& response) {
    response.set_header("Content-Type", "text/plain");
    response.write("Hello, World!");
  };

  my_api.get("/json") = [&](http_request& request, http_response& response) {
    response.write_json(s::message = "Hello, World!");
  };
  my_api.get("/db") = [&](http_request& request, http_response& response) {
    response.write_json(random_numbers.connect(request.yield).find_one(s::id = 1234).value());
  };

  my_api.get("/queries") = [&](http_request& request, http_response& response) {
    std::string N_str = request.get_parameters(s::N = std::optional<std::string>()).N.value_or("1");
    int N = atoi(N_str.c_str());
    
    N = std::max(1, std::min(N, 500));
    
    auto c = random_numbers.connect(request.yield);
    std::vector<decltype(random_numbers.all_fields())> numbers(N);
    for (int i = 0; i < N; i++)
      numbers[i] = c.find_one(s::id = 1 + rand() % 9999).value();

    response.write_json(numbers);
  };

  my_api.get("/updates") = [&](http_request& request, http_response& response) {
    std::string N_str = request.get_parameters(s::N = std::optional<std::string>()).N.value_or("1");
    int N = atoi(N_str.c_str());
    N = std::max(1, std::min(N, 500));

     
    auto c = random_numbers.connect(request.yield);
    auto& raw_c = c.backend_connection();

    std::vector<decltype(random_numbers.all_fields())> numbers(N);
    
#if TFB_MYSQL
    raw_c("START TRANSACTION");
#endif
    for (int i = 0; i < N; i++)
    {
      numbers[i] = c.find_one(s::id = 1 + rand() % 9999).value();
      numbers[i].randomNumber = 1 + rand() % 9999;
    }

    std::sort(numbers.begin(), numbers.end(), [] (auto a, auto b) { return a.id < b.id; });

#if TFB_MYSQL
    for (int i = 0; i < N; i++)
      c.update(numbers[i]);
    raw_c("COMMIT");
#elif TFB_PGSQL

    if (N == 20)
      raw_c.cached_statement
        ([N] {
           std::ostringstream ss;
           ss << "UPDATE World SET randomNumber=tmp.randomNumber FROM (VALUES ";
           for (int i = 0; i < N; i++)
             ss << "($" << i*2+1 << "::integer, $" << i*2+2 << "::integer) "<< (i == N-1 ? "": ",");
           ss << ") AS tmp(id, randomNumber) WHERE tmp.id = World.id";
           return ss.str();
         })(numbers);
    else
    {
      std::ostringstream ss;
      ss << "UPDATE World SET randomNumber=tmp.randomNumber FROM (VALUES ";
      for (int i = 0; i < N; i++)
        ss << "(" << numbers[i].id << ", " << numbers[i].randomNumber << ") "<< (i == N-1 ? "": ",");
      ss << ") AS tmp(id, randomNumber) WHERE tmp.id = World.id";
      raw_c(ss.str());
    }
    
#endif
    

    response.write_json(numbers);
  };

  my_api.get("/fortunes") = [&](http_request& request, http_response& response) {

    typedef decltype(fortunes.all_fields()) fortune;
    std::vector<fortune> table;

    auto c = fortunes.connect(request.yield);
    c.forall([&] (auto f) { table.emplace_back(f); });
    table.emplace_back(0, "Additional fortune added at request time.");

    std::sort(table.begin(), table.end(),
              [] (const fortune& a, const fortune& b) { return a.message < b.message; });

    char b[100000];
    li::output_buffer ss(b, sizeof(b));
 
    ss << "<!DOCTYPE html><html><head><title>Fortunes</title></head><body><table><tr><th>id</th><th>message</th></tr>";
    for(auto& f : table)
    {
      ss << "<tr><td>" << f.id << "</td><td>";
      escape_html_entities(ss, f.message); 
      ss << "</td></tr>";
    }
    ss << "</table></body></html>";

    response.set_header("Content-Type", "text/html; charset=utf-8");
    response.write(ss.to_string_view());
  };

  http_serve(my_api, port, s::nthreads = nprocs);
  
  return 0;

}
