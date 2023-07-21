    const char* data;
    int size;

    // Create first frame of two in message
    data = "Lorem ipsum";
    size = 11;
    vws_frame* f = vws_frame_new(data, size, BINARY_FRAME);

    // Modify frame to flip FIN off
    f->fin = 0;

    // Send frame. Note: this function takes ownership of frame and frees it. Do
    // NOT attempt to use frame pointer after making this call (without
    // allocating a new one).
    vws_frame_send(c, f);

    // Send the next frame as continuation. The FIN bit is automatically set to
    // 1, completing message.
    content = " dolor sit amet";
    size    = 15;
    vws_frame_send_data(cnx, data, size, CONTINUATION_FRAME)
