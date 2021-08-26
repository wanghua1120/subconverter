#include <string>
#include <iostream>
#include <libcron/Cron.h>

#include "../handler/interfaces.h"
#include "../handler/multithread.h"
#include "../server/webserver.h"
#include "../utils/logger.h"
#include "../utils/rapidjson_extra.h"
#include "../utils/system.h"
#include "script_quickjs.h"

extern bool gEnableCron;
extern string_array gCronTasks;
extern std::string gProxyConfig, gAccessToken;
extern int gCacheConfig;

libcron::Cron cron;

static std::string parseProxy(const std::string &source)
{
    std::string proxy = source;
    if(source == "SYSTEM")
        proxy = getSystemProxy();
    else if(source == "NONE")
        proxy = "";
    return proxy;
}

struct script_info
{
    std::string name;
    time_t begin_time = 0;
    time_t timeout = 0;
};

int timeout_checker(JSRuntime *rt, void *opaque)
{
    script_info info = *((script_info*)opaque);
    if(info.timeout != 0 && time(NULL) >= info.begin_time + info.timeout) /// timeout reached
    {
        writeLog(0, "Script '" + info.name + "' has exceeded timeout " + std::to_string(info.timeout) + ", terminate now.", LOG_LEVEL_WARNING);
        return 1;
    }
    return 0;
}

void refresh_schedule()
{
    cron.clear_schedules();
    for(std::string &x : gCronTasks)
    {
        string_array arguments = split(x, "`");
        if(arguments.size() < 3)
            continue;
        std::string &name = arguments[0], &cronexp = arguments[1], &path = arguments[2];
        cron.add_schedule(name, cronexp, [=](auto &)
        {
            qjs::Runtime runtime;
            qjs::Context context(runtime);
            try
            {
                script_runtime_init(runtime);
                script_context_init(context);
                defer(script_cleanup(context);)
                std::string proxy = parseProxy(gProxyConfig);
                std::string script = fetchFile(path, proxy, gCacheConfig);
                if(script.empty())
                {
                    writeLog(0, "Script '" + name + "' run failed: file is empty or not exist!", LOG_LEVEL_WARNING);
                    return;
                }
                script_info info;
                if(arguments.size() >= 4 && !arguments[3].empty())
                {
                    info.begin_time = time(NULL);
                    info.timeout = to_int(arguments[3], 0);
                    info.name = name;
                    JS_SetInterruptHandler(JS_GetRuntime(context.ctx), timeout_checker, &info);
                }
                context.eval(script);
            }
            catch (qjs::exception)
            {
                script_print_stack(context);
            }
        });
    }
}

std::string list_cron_schedule(RESPONSE_CALLBACK_ARGS)
{
    std::string &argument = request.argument;
    std::string token = getUrlArg(argument, "token");
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    writer.StartObject();
    if(token != gAccessToken)
    {
        response.status_code = 403;
        writer.Key("code");
        writer.Int(403);
        writer.Key("data");
        writer.String("Unauthorized");
        writer.EndObject();
        return sb.GetString();
    }
    writer.Key("code");
    writer.Int(200);
    writer.Key("tasks");
    writer.StartArray();
    for(std::string &x : gCronTasks)
    {
        string_array arguments = split(x, "`");
        if(arguments.size() < 3)
            continue;
        writer.StartObject();
        std::string &name = arguments[0], &cronexp = arguments[1], &path = arguments[2];
        writer.Key("name");
        writer.String(name.data());
        writer.Key("cronexp");
        writer.String(cronexp.data());
        writer.Key("path");
        writer.String(path.data());
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return sb.GetString();
}

size_t cron_tick()
{
    return cron.tick();
}
