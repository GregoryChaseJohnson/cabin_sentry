from http.server import BaseHTTPRequestHandler, HTTPServer
import json

class DiagnosticsHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers['Content-Length'])  # Read request size
        post_data = self.rfile.read(content_length)  # Read the incoming data
        print("\nReceived Diagnostics Data:")
        print(json.dumps(json.loads(post_data), indent=4))  # Pretty print the JSON
        
        # Send a response
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"status": "received"}')

# Run the server
server_address = ('0.0.0.0',  8071)  # Listen on all available interfaces, port 8070
httpd = HTTPServer(server_address, DiagnosticsHandler)
print("Server started on port 8071. Waiting for ESP32 data...")
httpd.serve_forever()
