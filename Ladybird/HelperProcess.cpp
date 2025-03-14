/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HelperProcess.h"
#include "Utilities.h"
#include <AK/Enumerate.h>
#include <LibCore/Process.h>
#include <LibWebView/ProcessManager.h>

enum class RegisterWithProcessManager {
    No,
    Yes,
};

template<typename ClientType, typename SpawnFunction>
static ErrorOr<NonnullRefPtr<ClientType>> launch_server_process_impl(
    StringView server_name,
    ReadonlySpan<ByteString> candidate_server_paths,
    Vector<ByteString> arguments,
    RegisterWithProcessManager register_with_process_manager,
    Ladybird::EnableCallgrindProfiling enable_callgrind_profiling,
    SpawnFunction&& spawn_function)
{
    if (enable_callgrind_profiling == Ladybird::EnableCallgrindProfiling::Yes) {
        arguments.prepend({
            "--tool=callgrind"sv,
            "--instr-atstart=no"sv,
            ""sv, // Placeholder for the process path.
        });
    }

    for (auto [i, path] : enumerate(candidate_server_paths)) {
        Core::ProcessSpawnOptions options { .name = server_name, .arguments = arguments };

        if (enable_callgrind_profiling == Ladybird::EnableCallgrindProfiling::Yes) {
            options.executable = "valgrind"sv;
            options.search_for_executable_in_path = true;
            arguments[2] = path;
        } else {
            options.executable = path;
        }

        auto result = spawn_function(options);

        if (!result.is_error()) {
            auto process = result.release_value();

            if constexpr (requires { process.client->set_pid(pid_t {}); })
                process.client->set_pid(process.process.pid());

            if (register_with_process_manager == RegisterWithProcessManager::Yes)
                WebView::ProcessManager::the().add_process(WebView::process_type_from_name(server_name), process.process.pid());

            if (enable_callgrind_profiling == Ladybird::EnableCallgrindProfiling::Yes) {
                dbgln();
                dbgln("\033[1;45mLaunched {} process under callgrind!\033[0m", server_name);
                dbgln("\033[100mRun `\033[4mcallgrind_control -i on\033[24m` to start instrumentation and `\033[4mcallgrind_control -i off\033[24m` stop it again.\033[0m");
                dbgln();
            }

            return move(process.client);
        }

        if (i == candidate_server_paths.size() - 1) {
            warnln("Could not launch any of {}: {}", candidate_server_paths, result.error());
            return result.release_error();
        }
    }

    VERIFY_NOT_REACHED();
}

template<typename ClientType, typename... ClientArguments>
static ErrorOr<NonnullRefPtr<ClientType>> launch_generic_server_process(
    StringView server_name,
    ReadonlySpan<ByteString> candidate_server_paths,
    Vector<ByteString> arguments,
    RegisterWithProcessManager register_with_process_manager,
    Ladybird::EnableCallgrindProfiling enable_callgrind_profiling,
    ClientArguments&&... client_arguments)
{
    return launch_server_process_impl<ClientType>(server_name, candidate_server_paths, move(arguments), register_with_process_manager, enable_callgrind_profiling, [&](auto options) {
        return Core::IPCProcess::spawn<ClientType>(move(options), forward<ClientArguments>(client_arguments)...);
    });
}

template<typename ClientType, typename... ClientArguments>
static ErrorOr<NonnullRefPtr<ClientType>> launch_singleton_server_process(
    StringView server_name,
    ReadonlySpan<ByteString> candidate_server_paths,
    Vector<ByteString> arguments,
    RegisterWithProcessManager register_with_process_manager,
    ClientArguments&&... client_arguments)
{
    return launch_server_process_impl<ClientType>(server_name, candidate_server_paths, move(arguments), register_with_process_manager, Ladybird::EnableCallgrindProfiling::No, [&](auto options) {
        return Core::IPCProcess::spawn_singleton<ClientType>(move(options), forward<ClientArguments>(client_arguments)...);
    });
}

ErrorOr<NonnullRefPtr<WebView::WebContentClient>> launch_web_content_process(
    WebView::ViewImplementation& view,
    ReadonlySpan<ByteString> candidate_web_content_paths,
    Ladybird::WebContentOptions const& web_content_options,
    Optional<IPC::File> request_server_socket)
{
    Vector<ByteString> arguments {
        "--command-line"sv,
        web_content_options.command_line.to_byte_string(),
        "--executable-path"sv,
        web_content_options.executable_path.to_byte_string(),
    };

    if (web_content_options.is_layout_test_mode == Ladybird::IsLayoutTestMode::Yes)
        arguments.append("--layout-test-mode"sv);
    if (web_content_options.use_lagom_networking == Ladybird::UseLagomNetworking::Yes)
        arguments.append("--use-lagom-networking"sv);
    if (web_content_options.enable_gpu_painting == Ladybird::EnableGPUPainting::Yes)
        arguments.append("--use-gpu-painting"sv);
    if (web_content_options.enable_experimental_cpu_transforms == Ladybird::EnableExperimentalCPUTransforms::Yes)
        arguments.append("--experimental-cpu-transforms"sv);
    if (web_content_options.wait_for_debugger == Ladybird::WaitForDebugger::Yes)
        arguments.append("--wait-for-debugger"sv);
    if (web_content_options.log_all_js_exceptions == Ladybird::LogAllJSExceptions::Yes)
        arguments.append("--log-all-js-exceptions"sv);
    if (web_content_options.enable_idl_tracing == Ladybird::EnableIDLTracing::Yes)
        arguments.append("--enable-idl-tracing"sv);
    if (web_content_options.enable_http_cache == Ladybird::EnableHTTPCache::Yes)
        arguments.append("--enable-http-cache"sv);
    if (web_content_options.expose_internals_object == Ladybird::ExposeInternalsObject::Yes)
        arguments.append("--expose-internals-object"sv);
    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }
    if (request_server_socket.has_value()) {
        arguments.append("--request-server-socket"sv);
        arguments.append(ByteString::number(request_server_socket->fd()));
    }

    return launch_generic_server_process<WebView::WebContentClient>("WebContent"sv, candidate_web_content_paths, move(arguments), RegisterWithProcessManager::No, web_content_options.enable_callgrind_profiling, view);
}

ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_image_decoder_process(ReadonlySpan<ByteString> candidate_image_decoder_paths)
{
    return launch_generic_server_process<ImageDecoderClient::Client>("ImageDecoder"sv, candidate_image_decoder_paths, {}, RegisterWithProcessManager::Yes, Ladybird::EnableCallgrindProfiling::No);
}

ErrorOr<NonnullRefPtr<Web::HTML::WebWorkerClient>> launch_web_worker_process(ReadonlySpan<ByteString> candidate_web_worker_paths, RefPtr<Protocol::RequestClient> request_client)
{
    Vector<ByteString> arguments;
    if (request_client) {
        auto socket = TRY(connect_new_request_server_client(*request_client));
        arguments.append("--request-server-socket"sv);
        arguments.append(ByteString::number(socket.fd()));
        arguments.append("--use-lagom-networking"sv);
        return launch_generic_server_process<Web::HTML::WebWorkerClient>("WebWorker"sv, candidate_web_worker_paths, move(arguments), RegisterWithProcessManager::Yes, Ladybird::EnableCallgrindProfiling::No);
    }

    return launch_generic_server_process<Web::HTML::WebWorkerClient>("WebWorker"sv, candidate_web_worker_paths, move(arguments), RegisterWithProcessManager::Yes, Ladybird::EnableCallgrindProfiling::No);
}

ErrorOr<NonnullRefPtr<Protocol::RequestClient>> launch_request_server_process(ReadonlySpan<ByteString> candidate_request_server_paths, StringView serenity_resource_root, Vector<ByteString> const& certificates)
{
    Vector<ByteString> arguments;

    if (!serenity_resource_root.is_empty()) {
        arguments.append("--serenity-resource-root"sv);
        arguments.append(serenity_resource_root);
    }

    for (auto const& certificate : certificates)
        arguments.append(ByteString::formatted("--certificate={}", certificate));

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_generic_server_process<Protocol::RequestClient>("RequestServer"sv, candidate_request_server_paths, move(arguments), RegisterWithProcessManager::Yes, Ladybird::EnableCallgrindProfiling::No);
}

ErrorOr<NonnullRefPtr<SQL::SQLClient>> launch_sql_server_process(ReadonlySpan<ByteString> candidate_sql_server_paths)
{
    Vector<ByteString> arguments;

    if (auto server = mach_server_name(); server.has_value()) {
        arguments.append("--mach-server-name"sv);
        arguments.append(server.value());
    }

    return launch_singleton_server_process<SQL::SQLClient>("SQLServer"sv, candidate_sql_server_paths, arguments, RegisterWithProcessManager::Yes);
}

ErrorOr<IPC::File> connect_new_request_server_client(Protocol::RequestClient& client)
{
    auto new_socket = client.send_sync_but_allow_failure<Messages::RequestServer::ConnectNewClient>();
    if (!new_socket)
        return Error::from_string_literal("Failed to connect to RequestServer");

    auto socket = new_socket->take_client_socket();
    TRY(socket.clear_close_on_exec());

    return socket;
}
