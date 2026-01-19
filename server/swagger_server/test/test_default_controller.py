# coding: utf-8

from __future__ import absolute_import

from flask import json
from six import BytesIO

from swagger_server.models.inline_response200 import InlineResponse200  # noqa: E501
from swagger_server.models.inline_response2001 import InlineResponse2001  # noqa: E501
from swagger_server.models.inline_response2002 import InlineResponse2002  # noqa: E501
from swagger_server.models.inline_response500 import InlineResponse500  # noqa: E501
from swagger_server.models.inline_response5001 import InlineResponse5001  # noqa: E501
from swagger_server.models.inline_response5002 import InlineResponse5002  # noqa: E501
from swagger_server.models.inline_response5003 import InlineResponse5003  # noqa: E501
from swagger_server.models.required_stream_configuration import RequiredStreamConfiguration  # noqa: E501
from swagger_server.models.stream_state import StreamState  # noqa: E501
from swagger_server.models.stream_update_body import StreamUpdateBody  # noqa: E501
from swagger_server.test import BaseTestCase


class TestDefaultController(BaseTestCase):
    """DefaultController integration test stubs"""

    def test_api_v1_stream_start_post(self):
        """Test case for api_v1_stream_start_post

        Start the video streaming with configuration.
        """
        body = RequiredStreamConfiguration()
        response = self.client.open(
            '/api/v1/stream/start',
            method='POST',
            data=json.dumps(body),
            content_type='application/json')
        self.assert200(response,
                       'Response body is : ' + response.data.decode('utf-8'))

    def test_api_v1_stream_state_get(self):
        """Test case for api_v1_stream_state_get

        Get the current state of the video streaming.
        """
        response = self.client.open(
            '/api/v1/stream/state',
            method='GET')
        self.assert200(response,
                       'Response body is : ' + response.data.decode('utf-8'))

    def test_api_v1_stream_stop_post(self):
        """Test case for api_v1_stream_stop_post

        Stop the video streaming.
        """
        response = self.client.open(
            '/api/v1/stream/stop',
            method='POST')
        self.assert200(response,
                       'Response body is : ' + response.data.decode('utf-8'))

    def test_api_v1_stream_update_put(self):
        """Test case for api_v1_stream_update_put

        Update the stream configuration dynamically.
        """
        body = StreamUpdateBody()
        response = self.client.open(
            '/api/v1/stream/update',
            method='PUT',
            data=json.dumps(body),
            content_type='application/json')
        self.assert200(response,
                       'Response body is : ' + response.data.decode('utf-8'))


if __name__ == '__main__':
    import unittest
    unittest.main()
