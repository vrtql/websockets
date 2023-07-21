    // Send a TEXT message
    vws_msg_send_text(cnx, "Hello, world!");

    // Receive websocket message
    vws_msg* reply = vws_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }

    // Send a BINARY message
    vws_msg_send_binary(cnx, "Hello, world!", 14);

    // Receive websocket message
    reply = vws_msg_recv(cnx);

    if (reply == NULL)
    {
        // There was no message received and it resulted in timeout
    }
    else
    {
        // Free message
        vws_msg_free(reply);
    }
