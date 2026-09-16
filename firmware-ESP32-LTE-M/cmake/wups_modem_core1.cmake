# Controlled, minimal source overlay for esp_modem 1.4.2. Both A/B images use
# this same generated source; CONFIG_WUPS_MODEM_CORE1 selects task affinity.
# Never patch the SDK or managed component in place. Unknown upstream inputs
# fail closed before the replacement can be compiled.
function(wups_generate_modem_uart component_dir output_cpp)
    set(uart_source "${component_dir}/src/esp_modem_uart.cpp")
    set(term_source "${component_dir}/src/esp_modem_term_uart.cpp")
    set(manifest "${component_dir}/idf_component.yml")
    file(STRINGS "${manifest}" modem_version REGEX "^version:")
    if(NOT modem_version STREQUAL "version: 1.4.2")
        message(FATAL_ERROR "B-UART overlay requires exactly esp_modem 1.4.2; review before updating")
    endif()
    file(SHA256 "${uart_source}" uart_hash)
    file(SHA256 "${term_source}" term_hash)
    if(NOT uart_hash STREQUAL "5614203eb5fc9218a8984644f05d0f3e3e429d442595cc007599313f636b5285" OR
       NOT term_hash STREQUAL "b8a576d4692e30292fe89ad862da66c69ac0452fe6597e0fb1b9808082a73e1e")
        message(FATAL_ERROR "B-UART overlay source hash changed; review UART creation and affinity tests before updating pins")
    endif()
    file(READ "${uart_source}" patched_source)

    set(old_constructor [=[    explicit uart_task(size_t stack_size, size_t priority, void *task_param, TaskFunction_t task_function) :
        task_handle(nullptr)
    {
        BaseType_t ret = xTaskCreate(task_function, "uart_task", stack_size, task_param, priority, &task_handle);
]=])
    set(new_constructor [=[    explicit uart_task(size_t stack_size, size_t priority, void *task_param, TaskFunction_t task_function, uart_port_t port) :
        task_handle(nullptr)
    {
        (void)port;
        BaseType_t ret;
#if CONFIG_WUPS_MODEM_CORE1
        if (port == UART_NUM_1) {
            ret = xTaskCreatePinnedToCore(task_function, "uart_task", stack_size, task_param, priority, &task_handle, 1);
        } else
#endif
        {
            ret = xTaskCreate(task_function, "uart_task", stack_size, task_param, priority, &task_handle);
        }
]=])
    set(old_call [=[        task_handle(config->task_stack_size, config->task_priority, this, s_task) {}]=])
    set(new_call [=[        task_handle(config->task_stack_size, config->task_priority, this, s_task, config->uart_config.port_num) {}]=])
    set(old_entry [=[        auto t = static_cast<UartTerminal *>(task_param);
        t->task();]=])
    set(new_entry [=[        auto t = static_cast<UartTerminal *>(task_param);
#if CONFIG_WUPS_PERF_DIAG
        BaseType_t affinity = xTaskGetCoreID(nullptr);
        ESP_LOGI(TAG, "affinity task=uart_task uart=%d running_core=%d affinity=%d priority=%u",
                 static_cast<int>(t->uart.port), xPortGetCoreID(),
                 affinity == tskNO_AFFINITY ? -1 : static_cast<int>(affinity),
                 static_cast<unsigned>(uxTaskPriorityGet(nullptr)));
#endif
        t->task();]=])
    foreach(part constructor call entry)
        string(FIND "${patched_source}" "${old_${part}}" anchor)
        if(anchor EQUAL -1)
            message(FATAL_ERROR "B-UART overlay missing ${part} anchor")
        endif()
        string(REPLACE "${old_${part}}" "${new_${part}}" patched_source "${patched_source}")
    endforeach()
    get_filename_component(output_dir "${output_cpp}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    if(EXISTS "${output_cpp}")
        file(READ "${output_cpp}" previous_source)
    endif()
    if(NOT patched_source STREQUAL previous_source)
        file(WRITE "${output_cpp}" "${patched_source}")
    endif()
endfunction()

function(wups_apply_modem_core1_overlay)
    idf_component_get_property(modem_dir espressif__esp_modem COMPONENT_DIR)
    idf_component_get_property(modem_lib espressif__esp_modem COMPONENT_LIB)
    set(output_cpp "${CMAKE_BINARY_DIR}/wups_modem_overlay/esp_modem_uart.cpp")
    wups_generate_modem_uart("${modem_dir}" "${output_cpp}")
    get_target_property(sources "${modem_lib}" SOURCES)
    set(replaced 0)
    set(new_sources)
    foreach(source IN LISTS sources)
        get_filename_component(absolute_source "${source}" ABSOLUTE BASE_DIR "${modem_dir}")
        if(absolute_source STREQUAL "${modem_dir}/src/esp_modem_uart.cpp")
            list(APPEND new_sources "${output_cpp}")
            math(EXPR replaced "${replaced} + 1")
        else()
            list(APPEND new_sources "${source}")
        endif()
    endforeach()
    if(NOT replaced EQUAL 1)
        message(FATAL_ERROR "B-UART overlay must replace exactly one modem UART source, found ${replaced}")
    endif()
    set_property(TARGET "${modem_lib}" PROPERTY SOURCES "${new_sources}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${modem_dir}/src/esp_modem_uart.cpp"
        "${modem_dir}/src/esp_modem_term_uart.cpp"
        "${modem_dir}/idf_component.yml")
    message(STATUS "B-UART: compiling reviewed esp_modem 1.4.2 UART overlay; CPU1=${CONFIG_WUPS_MODEM_CORE1}")
endfunction()
