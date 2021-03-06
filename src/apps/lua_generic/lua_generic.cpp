// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#include "enclave/app_interface.h"
#include "lua_interp/lua_args.h"
#include "lua_interp/lua_interp.h"
#include "lua_interp/lua_kv.h"
#include "lua_interp/tx_script_runner.h"
#include "node/rpc/user_frontend.h"

#include <memory>
#include <vector>

namespace ccfapp
{
  using namespace kv;
  using namespace ccf;
  using namespace lua;

  using GenericTable = kv::Map<nlohmann::json, nlohmann::json>;

  class AppTsr : public TxScriptRunner
  {
  private:
    void add_error_codes(
      lua_State* l, char const* table_name = "error_codes") const
    {
      // Get env table on top of stack
      lua_getglobal(l, env_table_name);
      if (lua_isnil(l, -1))
      {
        LOG_FAIL_FMT(
          "There is no env table '{}', skipping creation of error codes table "
          "'{}'",
          env_table_name,
          table_name);
      }

      // Create error_codes table
      lua_newtable(l);

#define XX(Num, Name, String) \
  lua_pushinteger(l, Num); \
  lua_setfield(l, -2, #Name);

      HTTP_STATUS_MAP(XX);

#undef XX

      lua_setfield(l, -2, table_name);

      // Remove env table from stack
      lua_pop(l, 1);
    }

    void setup_environment(
      lua::Interpreter& li,
      const std::optional<Script>& env_script) const override
    {
      auto l = li.get_state();

      // Create env table
      lua_newtable(l);
      lua_setglobal(l, env_table_name);

      add_error_codes(li.get_state());

      TxScriptRunner::setup_environment(li, env_script);
    }

    const std::vector<GenericTable*> app_tables;
    void add_custom_tables(
      lua::Interpreter& li, kv::Tx& tx, int& n_registered_tables) const override
    {
      n_registered_tables++;
      TableCreator<false>::create(li, tx, app_tables);
    }

  public:
    AppTsr(NetworkTables& network, std::vector<GenericTable*> app_tables) :
      TxScriptRunner(network),
      app_tables(app_tables)
    {}
  };

  class LuaHandlers : public UserEndpointRegistry
  {
  private:
    NetworkTables& network;
    std::unique_ptr<AppTsr> tsr;

  public:
    LuaHandlers(NetworkTables& network, const uint16_t n_tables = 8) :
      UserEndpointRegistry(network),
      network(network)
    {
      auto& tables = *network.tables;

      // create public and private app tables (2x n_tables in total)
      std::vector<GenericTable*> app_tables(n_tables * 2);
      for (uint16_t i = 0; i < n_tables; i++)
      {
        const auto suffix = std::to_string(i);
        app_tables[i] = &tables.create<GenericTable>("priv" + suffix);
        app_tables[i + n_tables] = &tables.create<GenericTable>("pub" + suffix);
      }
      tsr = std::make_unique<AppTsr>(network, app_tables);

      auto default_handler = [this](EndpointContext& args, nlohmann::json&&) {
        const auto method = args.rpc_ctx->get_method();
        const auto local_method = method.substr(method.find_first_not_of('/'));
        if (local_method == UserScriptIds::ENV_HANDLER)
        {
          return make_error(
            HTTP_STATUS_NOT_FOUND,
            fmt::format("Cannot call environment script ('{}')", local_method));
        }

        const auto scripts = args.tx.get_view(this->network.app_scripts);

        // try find script for method
        // - First try a script called "foo"
        // - If that fails, try a script called "POST foo"
        auto handler_script = scripts->get(local_method);
        if (!handler_script)
        {
          const auto verb_prefixed = fmt::format(
            "{} {}", args.rpc_ctx->get_request_verb().c_str(), local_method);
          handler_script = scripts->get(verb_prefixed);
          if (!handler_script)
          {
            return make_error(
              HTTP_STATUS_NOT_FOUND,
              fmt::format("No handler script found for '{}'", verb_prefixed));
          }
        }

        auto response = tsr->run<nlohmann::json>(
          args.tx,
          {*handler_script,
           {},
           WlIds::USER_APP_CAN_READ_ONLY,
           scripts->get(UserScriptIds::ENV_HANDLER)},
          // vvv arguments to the script vvv
          args);

        auto err_it = response.find("error");
        if (err_it == response.end())
        {
          auto result_it = response.find("result");
          if (result_it == response.end())
          {
            // Response contains neither result nor error. It may not even be an
            // object. We assume the entire response is a successful result.
            return make_success(std::move(response));
          }
          else
          {
            return make_success(std::move(*result_it));
          }
        }
        else
        {
          http_status err_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
          std::string msg;

          if (err_it->is_object())
          {
            auto err_code_it = err_it->find("code");
            if (err_code_it != err_it->end())
            {
              err_code = *err_code_it;
            }

            auto err_message_it = err_it->find("message");
            if (err_message_it != err_it->end())
            {
              msg = *err_message_it;
            }
          }

          return make_error(err_code, std::move(msg));
        }
      };

      set_default(json_adapter(default_handler));
    }

    static std::pair<http_method, std::string> split_script_key(
      const std::string& key)
    {
      size_t s = key.find(' ');
      if (s != std::string::npos)
      {
        return std::make_pair(
          http::http_method_from_str(key.substr(0, s).c_str()),
          key.substr(s + 1, key.size() - (s + 1)));
      }
      else
      {
        return std::make_pair(HTTP_POST, key);
      }
    }

    // Since we do our own dispatch within the default handler, report the
    // supported methods here
    void build_api(nlohmann::json& document, kv::Tx& tx) override
    {
      UserEndpointRegistry::build_api(document, tx);

      auto scripts = tx.get_view(this->network.app_scripts);
      scripts->foreach([&document](const auto& key, const auto&) {
        const auto [verb, method] = split_script_key(key);

        ds::openapi::path_operation(ds::openapi::path(document, method), verb);
        return true;
      });
    }

    nlohmann::json get_endpoint_schema(
      kv::Tx& tx, const GetSchema::In& in) override
    {
      auto j = UserEndpointRegistry::get_endpoint_schema(tx, in);

      auto scripts = tx.get_view(this->network.app_scripts);
      scripts->foreach([&j, &in](const auto& key, const auto&) {
        const auto [verb, method] = split_script_key(key);

        if (in.method == method)
        {
          std::string verb_name = http_method_str(verb);
          nonstd::to_lower(verb_name);
          // We have no schema for JS endpoints, but populate the object if we
          // know about them
          GetSchema::Out out;
          out.params_schema.schema = nullptr;
          out.result_schema.schema = nullptr;
          j[verb_name] = out;
        }

        return true;
      });

      return j;
    }
  };

  class Lua : public ccf::UserRpcFrontend
  {
  private:
    LuaHandlers lua_handlers;

  public:
    Lua(NetworkTables& network) :
      ccf::UserRpcFrontend(*network.tables, lua_handlers),
      lua_handlers(network)
    {}
  };

  std::shared_ptr<ccf::UserRpcFrontend> get_rpc_handler(
    NetworkTables& network, ccfapp::AbstractNodeContext&)
  {
    return std::make_shared<Lua>(network);
  }
} // namespace ccfapp
