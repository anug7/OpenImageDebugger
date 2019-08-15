/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2019 GDB ImageWatch contributors
 * (github.com/csantosbh/gdb-imagewatch/)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <deque>
#include <iostream>
#include <string>

#include <signal.h>

#include "debuggerinterface/preprocessor_directives.h"
#include "debuggerinterface/python_native_interface.h"
#include "giw_bridge.h"
#include "ipc/message_exchange.h"

#include <QDataStream>
#include <QProcess>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>


using namespace std;

struct UiMessage
{
    virtual ~UiMessage()
    {
    }
};

struct GetObservedSymbolsResponseMessage : public UiMessage
{
    std::deque<string> observed_symbols;
};

struct PlotBufferRequestMessage : public UiMessage
{
    std::string buffer_name;
};

class PyGILRAII
{
  public:
    PyGILRAII()
    {
        _py_gil_state = PyGILState_Ensure();
    }
    PyGILRAII(const PyGILRAII&)  = delete;
    PyGILRAII(const PyGILRAII&&) = delete;

    PyGILRAII& operator=(const PyGILRAII&) = delete;
    PyGILRAII& operator=(const PyGILRAII&&) = delete;

    ~PyGILRAII()
    {
        PyGILState_Release(_py_gil_state);
    }

  private:
    PyGILState_STATE _py_gil_state;
};

class GiwBridge
{
  public:
    GiwBridge(int (*plot_callback)(const char*))
        : client_(nullptr)
        , plot_callback_(plot_callback)
    {
    }

    bool start()
    {
        // Initialize server
        if (!server_.listen(QHostAddress::Any)) {
            // TODO escalate error
            cerr << "[giw] Could not start TCP server" << endl;
            return false;
        }

        // TODO get proper binary path at runtime
        QString program =
            "/Users/claudio.fernandes/workspace/pessoal/gdb-imagewatch/build/"
            "src/giwwindow.app/Contents/MacOS/giwwindow";
        QStringList arguments;
        arguments << "-style" << "fusion"
                  << "-p" << QString::number(server_.serverPort());
        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.start(program, arguments);

        wait_for_client();

        return client_ != nullptr;
    }

    bool is_window_ready()
    {
        return client_ != nullptr && process_.processId() != 0 &&
               kill(static_cast<pid_t>(process_.processId()), 0) == 0;
    }

    deque<string> get_observed_symbols()
    {
        assert(client_ != nullptr);

        MessageComposer message_composer;
        message_composer.push(MessageType::GetObservedSymbols).send(client_);

        auto response = fetch_message(MessageType::GetObservedSymbolsResponse);
        if (response != nullptr) {
            return static_cast<GetObservedSymbolsResponseMessage*>(
                       response.get())
                ->observed_symbols;
        } else {
            return {};
        }
    }


    void set_available_symbols(const deque<string>& available_vars)
    {
        assert(client_ != nullptr);

        MessageComposer message_composer;
        message_composer.push(MessageType::SetAvailableSymbols)
            .push(available_vars)
            .send(client_);
    }

    void run_event_loop()
    {
        try_read_incoming_messages(static_cast<int>(1000.0 / 5.0));

        unique_ptr<UiMessage> plot_request_message;
        while ((plot_request_message = try_get_stored_message(
                    MessageType::PlotBufferRequest)) != nullptr) {
            const PlotBufferRequestMessage* msg =
                dynamic_cast<PlotBufferRequestMessage*>(
                    plot_request_message.get());
            plot_callback_(msg->buffer_name.c_str());
        }
    }

    void plot_buffer(const string& variable_name_str,
                     const string& display_name_str,
                     const string& pixel_layout_str,
                     bool transpose_buffer,
                     int buff_width,
                     int buff_height,
                     int buff_channels,
                     int buff_stride,
                     BufferType buff_type,
                     uint8_t* buff_ptr,
                     size_t buff_length)
    {
        MessageComposer message_composer;
        message_composer.push(MessageType::PlotBufferContents)
            .push(variable_name_str)
            .push(display_name_str)
            .push(pixel_layout_str)
            .push(transpose_buffer)
            .push(buff_width)
            .push(buff_height)
            .push(buff_channels)
            .push(buff_stride)
            .push(buff_type)
            .push(buff_ptr, buff_length)
            .send(client_);
    }

    ~GiwBridge()
    {
        process_.kill();
    }

  private:
    QProcess process_;
    QTcpServer server_;
    QTcpSocket* client_;

    int (*plot_callback_)(const char*);

    std::map<MessageType, std::unique_ptr<UiMessage>> received_messages_;

    std::unique_ptr<UiMessage>
    try_get_stored_message(const MessageType& msg_type)
    {
        auto find_msg_handler = received_messages_.find(msg_type);

        if (find_msg_handler != received_messages_.end()) {
            unique_ptr<UiMessage> result = std::move(find_msg_handler->second);
            received_messages_.erase(find_msg_handler);
            return result;
        }

        return nullptr;
    }


    void try_read_incoming_messages(int msecs = 3000)
    {
        assert(client_ != nullptr);

        do {
            client_->waitForReadyRead(msecs);

            if (client_->bytesAvailable() == 0) {
                break;
            }

            MessageType header;
            client_->read(reinterpret_cast<char*>(&header),
                          static_cast<qint64>(sizeof(header)));

            switch (header) {
            case MessageType::PlotBufferRequest:
                received_messages_[header] = decode_plot_buffer_request();
                break;
            case MessageType::GetObservedSymbolsResponse:
                received_messages_[header] =
                    decode_get_observed_symbols_response();
                break;
            default:
                cerr << "[giw] Received message with incorrect header" << endl;
                break;
            }
        } while (client_->bytesAvailable() > 0);
    }


    unique_ptr<UiMessage> decode_plot_buffer_request()
    {
        assert(client_ != nullptr);

        auto response = new PlotBufferRequestMessage();
        MessageDecoder message_decoder(client_);
        message_decoder.read(response->buffer_name);
        return unique_ptr<UiMessage>(response);
    }

    unique_ptr<UiMessage> decode_get_observed_symbols_response()
    {
        assert(client_ != nullptr);

        auto response = new GetObservedSymbolsResponseMessage();

        MessageDecoder message_decoder(client_);
        message_decoder.read<std::deque<std::string>, std::string>(
            response->observed_symbols);

        return unique_ptr<UiMessage>(response);
    }

    std::unique_ptr<UiMessage> fetch_message(const MessageType& msg_type)
    {
        // Return message if it was already received before
        auto result = try_get_stored_message(msg_type);

        if (result != nullptr) {
            return result;
        }

        // Try to fetch message
        try_read_incoming_messages();

        return try_get_stored_message(msg_type);
    }


    void wait_for_client()
    {
        if (client_ == nullptr) {
            if (!server_.waitForNewConnection(10000)) {
                cerr << "[giw] No clients connected to ImageWatch server"
                     << endl;
            }
            client_ = server_.nextPendingConnection();
        }
    }
};


AppHandler giw_initialize(int (*plot_callback)(const char*))
{
    PyGILRAII py_gil_raii;

    GiwBridge* app = new GiwBridge(plot_callback);
    return static_cast<AppHandler>(app);
}


void giw_cleanup(AppHandler handler)
{
    PyGILRAII py_gil_raii;

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_terminate received null application handler");
        return;
    }

    delete app;
}


void giw_exec(AppHandler handler)
{
    PyGILRAII py_gil_raii;

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_exec received null application handler");
        return;
    }

    app->start();
}


int giw_is_window_ready(AppHandler handler)
{
    PyGILRAII py_gil_raii;

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_exec received null application handler");
        return 0;
    }

    return app->is_window_ready();
}


PyObject* giw_get_observed_buffers(AppHandler handler)
{
    PyGILRAII py_gil_raii;

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_Exception,
                           "giw_get_observed_buffers received null "
                           "application handler");
        return nullptr;
    }

    auto observed_symbols = app->get_observed_symbols();
    PyObject* py_observed_symbols =
        PyList_New(static_cast<Py_ssize_t>(observed_symbols.size()));

    int observed_symbols_sentinel = static_cast<int>(observed_symbols.size());
    for (int i = 0; i < observed_symbols_sentinel; ++i) {
        string symbol_name       = observed_symbols[i];
        PyObject* py_symbol_name = PyBytes_FromString(symbol_name.c_str());

        if (py_symbol_name == nullptr) {
            Py_DECREF(py_observed_symbols);
            return nullptr;
        }

        PyList_SetItem(py_observed_symbols, i, py_symbol_name);
    }

    return py_observed_symbols;
}


void giw_set_available_symbols(AppHandler handler, PyObject* available_vars_py)
{
    PyGILRAII py_gil_raii;

    assert(PyList_Check(available_vars_py));

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_set_available_symbols received null "
                           "application handler");
        return;
    }

    deque<string> available_vars_stl;
    for (Py_ssize_t pos = 0; pos < PyList_Size(available_vars_py); ++pos) {
        string var_name_str;
        PyObject* listItem = PyList_GetItem(available_vars_py, pos);
        copy_py_string(var_name_str, listItem);
        available_vars_stl.push_back(var_name_str);
    }

    app->set_available_symbols(available_vars_stl);
}


void giw_run_event_loop(AppHandler handler)
{
    PyGILRAII py_gil_raii;

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_run_event_loop received null application "
                           "handler");
        return;
    }

    app->run_event_loop();
}


void giw_plot_buffer(AppHandler handler, PyObject* buffer_metadata)
{
    PyGILRAII py_gil_raii;


    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_plot_buffer received null application handler");
        return;
    }

    if (!PyDict_Check(buffer_metadata)) {
        RAISE_PY_EXCEPTION(PyExc_TypeError,
                           "Invalid object given to plot_buffer (was expecting"
                           " a dict).");
        return;
    }

    /*
     * Get required fields
     */
    PyObject* py_variable_name =
        PyDict_GetItemString(buffer_metadata, "variable_name");
    PyObject* py_display_name =
        PyDict_GetItemString(buffer_metadata, "display_name");
    PyObject* py_pointer = PyDict_GetItemString(buffer_metadata, "pointer");
    PyObject* py_width   = PyDict_GetItemString(buffer_metadata, "width");
    PyObject* py_height   = PyDict_GetItemString(buffer_metadata, "height");
    PyObject* py_channels = PyDict_GetItemString(buffer_metadata, "channels");
    PyObject* py_type     = PyDict_GetItemString(buffer_metadata, "type");
    PyObject* py_row_stride =
        PyDict_GetItemString(buffer_metadata, "row_stride");
    PyObject* py_pixel_layout =
        PyDict_GetItemString(buffer_metadata, "pixel_layout");

    /*
     * Get optional fields
     */
    PyObject* py_transpose_buffer =
        PyDict_GetItemString(buffer_metadata, "transpose_buffer");
    bool transpose_buffer = false;
    if (py_transpose_buffer != nullptr) {
        CHECK_FIELD_TYPE(transpose_buffer, PyBool_Check, "transpose_buffer");
        transpose_buffer = PyObject_IsTrue(py_transpose_buffer);
    }

    /*
     * Check if expected fields were provided
     */
    CHECK_FIELD_PROVIDED(variable_name, "plot_buffer");
    CHECK_FIELD_PROVIDED(display_name, "plot_buffer");
    CHECK_FIELD_PROVIDED(pointer, "plot_buffer");
    CHECK_FIELD_PROVIDED(width, "plot_buffer");
    CHECK_FIELD_PROVIDED(height, "plot_buffer");
    CHECK_FIELD_PROVIDED(channels, "plot_buffer");
    CHECK_FIELD_PROVIDED(type, "plot_buffer");
    CHECK_FIELD_PROVIDED(row_stride, "plot_buffer");
    CHECK_FIELD_PROVIDED(pixel_layout, "plot_buffer");

    /*
     * Check if expected fields have the correct types
     */
    CHECK_FIELD_TYPE(variable_name, check_py_string_type, "plot_buffer");
    CHECK_FIELD_TYPE(display_name, check_py_string_type, "plot_buffer");
    CHECK_FIELD_TYPE(width, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(height, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(channels, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(type, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(row_stride, PY_INT_CHECK_FUNC, "plot_buffer");
    CHECK_FIELD_TYPE(pixel_layout, check_py_string_type, "plot_buffer");


    // Retrieve pointer to buffer
    uint8_t* buff_ptr;
#if PY_MAJOR_VERSION == 3
    if (PyMemoryView_Check(py_pointer) != 0) {
        buff_ptr =
            reinterpret_cast<uint8_t*>(get_c_ptr_from_py_buffer(py_pointer));
#elif PY_MAJOR_VERSION == 2
    auto pybuffer_deleter = [](Py_buffer* buff) {
        PyBuffer_Release(buff);
        delete buff;
    };
    std::unique_ptr<Py_buffer, decltype(pybuffer_deleter)> py_buff(
        nullptr, pybuffer_deleter);

    if (PyBuffer_Check(py_pointer) != 0) {
        py_buff.reset(new Py_buffer());
        PyObject_GetBuffer(py_pointer, py_buff.get(), PyBUF_SIMPLE);
        buff_ptr = reinterpret_cast<uint8_t*>(py_buff->buf);
#endif
    } else {
        RAISE_PY_EXCEPTION(PyExc_TypeError,
                           "Could not retrieve C pointer to provided buffer");
        return;
    }

    /*
     * Send buffer contents
     */
    string variable_name_str;
    string display_name_str;
    string pixel_layout_str;

    copy_py_string(variable_name_str, py_variable_name);
    copy_py_string(display_name_str, py_display_name);
    copy_py_string(pixel_layout_str, py_pixel_layout);

    int buff_width    = get_py_int(py_width);
    int buff_height   = get_py_int(py_height);
    int buff_channels = get_py_int(py_channels);
    int buff_stride   = get_py_int(py_row_stride);

    BufferType buff_type = static_cast<BufferType>(get_py_int(py_type));

    size_t buff_length =
        static_cast<size_t>(buff_width * buff_height * buff_channels) *
        typesize(buff_type);

    app->plot_buffer(variable_name_str,
                     display_name_str,
                     pixel_layout_str,
                     transpose_buffer,
                     buff_width,
                     buff_height,
                     buff_channels,
                     buff_stride,
                     buff_type,
                     buff_ptr,
                     buff_length);
}
