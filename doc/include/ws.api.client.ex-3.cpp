    // Send a TEXT frame
    vws_frame_send_text(cnx, "Hello, world!");

    // Receive frame
    vws_frame* reply = vws_frame_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_frame_free(reply);
    }

    // Send a BINARY frame
    vws_frame_send_binary(cnx, "Hello, world!", 14);

    // Receive frame
    reply = vws_frame_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free frame
        vws_frame_free(reply);
    }
