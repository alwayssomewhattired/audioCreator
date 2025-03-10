
#define _USE_MATH_DEFINES


// websockets (windows)
//#include <winsock.h> 


#include <iostream>
#include <vector>
#include <sndfile.h>
#include <fftw3.h>
#include <fstream>
#include <cmath>

// Unique Key
#include <iomanip>


// websockets (linux)
//#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

// AWS
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/putObjectRequest.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/DateTime.h>

// SSL/TLS
#include <websocketpp/config/asio_client.hpp>

#include <string>
#include <cstdlib>
#include <memory>
#include <functional>

#include <websocketpp/config/asio.hpp>
#include <websocketpp/common/thread.hpp>
#include <websocketpp/common/memory.hpp>
#include <websocketpp/transport/asio/security/tls.hpp>

#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>

// JSON for messaging
#include <json/json.h>



// to-do

// if source audio is not stereo, process it instead of throwing an error.

// I think I'm applying window effect to the actual signal. The resulting audio fades in and out. I should apply the 
// window to a reference to the actual audio instead.

// samples clip at times (past -1 or 1). Might be because of uninitialize sampleStorage size.
// initiaized sampleStorage size and still have issue...

// increase amount of bins. 
// you can do this by increasing fft size (increases time it takes).
// You can also change the bins from linear to exponential.

// 5295744 (2 seconds)
// if sample storage exceeds this size, cancel everything and make the wav with it's data.

// add a normalisation feature that compresses the audio.

// It takes a while to fetch audio from s3 and make the audio and then read the audio... more efficieny would be great... store in cache
// It also takes a while to upload to s3.

// Make a unique object key for every upload and send the key to server.


// DEVELOPMENT
//typedef websocketpp::client<websocketpp::config::asio_client> client;
//typedef websocketpp::connection_hdl connection_hdl;

// PRODUCTION
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::connection_hdl connection_hdl;

// SSL/TLS
std::shared_ptr<boost::asio::ssl::context> on_tls_init(websocketpp::connection_hdl hdl) {
	std::shared_ptr<boost::asio::ssl::context> ctx =
		std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);

	try {
		ctx->set_options(boost::asio::ssl::context::default_workarounds |
			boost::asio::ssl::context::no_sslv2 |
			boost::asio::ssl::context::no_sslv3 |
			boost::asio::ssl::context::single_dh_use);

		ctx->set_verify_mode(boost::asio::ssl::verify_none); // disable cert verification
	}
	catch (std::exception& e) {
		std::cerr << "Error in context initialization: " << e.what() << std::endl;
	}

	return ctx;
}

bool connected = false;

std::atomic<bool> task_done(false);

// main thread blocking
std::mutex mtx;
std::condition_variable cv;
bool condition_met = false;

client c;
websocketpp::connection_hdl hdl_global;  // Store connection handle for sending messages



// Callback when a message is received
void on_message(connection_hdl hdl, client::message_ptr msg) {
	//std::string payload = msg->get_payload(); // Get the message content
	// Parse JSON response
	Json::Value root;
	Json::CharReaderBuilder reader;
	std::string errs;

	std::istringstream s(msg->get_payload());
	if (Json::parseFromStream(reader, s, &root, &errs)) {
		std::string received_message = root.get("message", "").asString();
		std::cout << "Message: " << received_message << std::endl;
		if (received_message == "source_upload") {
			std::cout << "Source has been uploaded!" << std::endl;
			std::lock_guard<std::mutex> lock(mtx);
			condition_met = true;
			cv.notify_one();
		}
	}
	else {
		std::cerr << "Failed to parse JSON: " << errs << std::endl;
	}
	return;
}

// Callback when connection is established
void on_open(connection_hdl hdl) {
	std::cout << "Connected to server!" << std::endl;
	hdl_global = hdl;  // Store the connection handle
	connected = true;

}

// Callback for connection failure
void on_fail(connection_hdl hdl) {
	std::cout << "Connection failed!" << std::endl;
}

// Product length in samples
int productDurationSamples = 96000;

// timestamp-based id
std::string generateTimestampID() {
	auto now = std::chrono::system_clock::now();
	auto time_t_now = std::chrono::system_clock::to_time_t(now);
	auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

	std::tm tm_now;
	localtime_s(&tm_now, &time_t_now); // Windows-safe localtime

	std::stringstream ss;
	ss << std::put_time(&tm_now, "%Y%m%d_%H%M%S") // Format: YYYYMMDD_HHMMSS
		<< "_" << std::setfill('0') << std::setw(3) << milliseconds.count() //Add milliseconds
		<< ".wav"; //.wav ofc :)

	return ss.str();
}

int networking(std::vector<double> sampleStorage) {
	try {


		websocketpp::lib::error_code ec;
		//client::connection_ptr con = c.get_connection("ws://localhost:9000", ec); // This is for local development
		client::connection_ptr con = c.get_connection("wss://www.samplerinfinite.com", ec); // This is for production

		if (ec) {
			std::cout << "Connection failed: " << ec.message() << std::endl;
			return 1;
		}

		c.connect(con);

		// thread splitting
		std::thread websocket_thread([&]() { c.run(); });

		// now the main thread is free to do other work
		std::this_thread::sleep_for(std::chrono::seconds(2));
		std::cout << "Main thread is still running!" << std::endl;

		// Send a message while the WebSocket loop is running
		while (!connected) {
			std::this_thread::sleep_for(std::chrono::seconds(1));  // Avoid 100% CPU usage
		}

		// Join the thread when product is ready
		if (task_done == true) {
			websocket_thread.join();
		}


		websocket_thread.detach();

	}
	catch (const std::exception& e) {
		std::cout << "Exception: " << e.what() << std::endl;
	}

	return 0;
}

// function to read mono audio... might be fucked 
std::vector<double> read_mono_file(const std::string filename)
{
	SF_INFO sfInfo;

	// open input mono audio file
	SNDFILE* inFile = sf_open(filename.c_str(), SFM_READ, &sfInfo);
	if (!inFile) {
		std::cerr << "Error opening audio file: " << filename << std::endl;
		return {};
	}

	//error if input audio file is not stereo
	if (sfInfo.channels != 1) {
		std::cerr << "Input file is not mono!!" << std::endl;
		return {};
	}

	size_t numFrames = sfInfo.frames;
	int numChannels = sfInfo.channels;

	// allocate buffer for reading mono data
	std::vector<double> monoData(numFrames * numChannels);

	//read the mono data into the mono buffer
	sf_read_double(inFile, monoData.data(), numFrames * numChannels);

	//close the input file
	sf_close(inFile);

	return monoData;

}

//function to read + convert local source audio to mono
std::vector<double> read_audio_file(const std::string& filename)
{

	SF_INFO sfInfo;

	// open input stereo audio file
	SNDFILE* inFile = sf_open(filename.c_str(), SFM_READ, &sfInfo);
	if (!inFile) {
		std::cerr << "Error opening audio file: " << filename << std::endl;
		return {};
	}

	//error if input audio file is not stereo
	if (sfInfo.channels != 2) {
		std::cerr << "Input file is not stereo!!" << std::endl;
		return {};
	}

	size_t numFrames = sfInfo.frames;
	int numChannels = sfInfo.channels;

	// allocate buffer for reading stereo data
	std::vector<double> stereoData(numFrames * numChannels);

	//read the stereo data into the stereo buffer
	sf_read_double(inFile, stereoData.data(), numFrames * numChannels);

	//close the input file
	sf_close(inFile);

	// allocate the buffer for mono data
	std::vector<double> monoData(numFrames);

	//convert stereo to mono by averaging the left and right channels
	for (size_t i = 0; i < numFrames; ++i) {
		// left channel is at index 2*i, right channel is at index 2*i + 1
		monoData[i] = (stereoData[2 * i] + stereoData[2 * i + 1] / 2);
	}

	sf_close(inFile);

	return monoData;
}



// create function to apply a hanning window s
void applyHanningWindow(std::vector<double>& signal, int n) {
	for (int i = 0; i < n; i++) {
		double hannValue = 0.5 * (1 - cos(2 * M_PI * i / (n - 1)));
		signal[i] *= hannValue;
	}
}

std::vector<double> sampleStorage;

// create a function for finding and storing prominent frequency samples
bool isGreaterThanAll(std::vector<double>& vec, double value, int counter,
	std::vector<double>& samples, int CHUNK_SIZE) {
	for (double element : vec) {
		if (value < element || value == 0) {
			return false;
		} 
	}
	int CHUNK_SLICE_START = counter * CHUNK_SIZE;
	int CHUNK_SLICE_END = CHUNK_SLICE_START + CHUNK_SIZE;
	//sampleStorage.reserve(5295743); // This literally does nothing lol
	//store and normalize samples
	for (int i = CHUNK_SLICE_START; i < CHUNK_SLICE_END; ++i) {
		if (samples[i] < -1) {
			samples[i] = -1;
		}
		else if (samples[i] > 1) {
			samples[i] = 1;
		}
		//sampleStorage.emplace_back(samples[i]); // This might be breaking					I don't know
		sampleStorage.push_back(samples[i]); // This might fix it							which is better...
		//std::cout << "stored samples!!!: " << samples[i] << std::endl;
	}
	return true;
}




void ReadAudioFileFromS3(const Aws::String& bucketName, const Aws::String& objectKey) {
	Aws::SDKOptions options;
	Aws::InitAPI(options);
	{

		// Create S3 client
		Aws::Client::ClientConfiguration config;
		config.region = "us-east-2";
		Aws::S3::S3Client s3_client(config);

		std::cout << "hello" << std::endl;
		// Create request
		Aws::S3::Model::GetObjectRequest request;
		request.SetBucket(bucketName);
		request.SetKey(objectKey);


		// Execute request
		auto outcome = s3_client.GetObject(request);
		if (outcome.IsSuccess()) {
			auto& stream = outcome.GetResult().GetBody();
			std::vector<char> s3Data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

			std::cout << "Successfully read " << s3Data.size() << " bytes from S3" << std::endl;
			
			// Save audio to temporary file (libsndfile requires a file)
			std::string tempFile = "temp_audio.mp3";
			std::ofstream outFile(tempFile, std::ios::binary);
			outFile.write(s3Data.data(), s3Data.size());
			outFile.close();
		}
		else {
			std::cerr << "Failed to read file from S3: " << outcome.GetError().GetMessage() << std::endl;
		}
	}
	Aws::ShutdownAPI(options);
}

int main()
{
	c.init_asio();
	// PROD

	// Enable error logging for all error levels
	c.set_error_channels(websocketpp::log::elevel::all);

	// Enable access logging for all access levels (includes connection open, close, frames, etc.)
	c.set_access_channels(websocketpp::log::alevel::all);
	c.set_tls_init_handler(std::bind(&on_tls_init, std::placeholders::_1));
	c.set_message_handler(&on_message);
	c.set_open_handler(&on_open);
	c.set_fail_handler(&on_fail);

		const Aws::String bucketName = "firstdemoby";
		const Aws::String objectKey = "fetch-test.mp3";


		const char* outputName = "output.wav";
		int sampleRate = 44100;
		int channels = 1;
		int format = SF_FORMAT_WAV | SF_FORMAT_PCM_32; // 32 bit pcm wav

		SNDFILE* infiniteFile;
		SF_INFO sf_info;
		sf_info.samplerate = sampleRate;
		sf_info.channels = channels;
		sf_info.format = format;
		int chunk_size = 2048;

		//WEBSOCKET (LINUX)
		networking(sampleStorage);

	while (sampleStorage.size() < productDurationSamples) {
		std::cout << sampleStorage.size() << std::endl;

		if (sampleStorage.size() >= productDurationSamples) {
			Json::Value message;
			message["action"] = "sendMessage";
			message["body"] = "finish_now";

			// Convert the JSON object to a string
			Json::StreamWriterBuilder writer;
			std::string message_str = Json::writeString(writer, message);
			std::cout << message_str << std::endl;
			c.send(hdl_global, message_str, websocketpp::frame::opcode::text);
			task_done = true;
		}

		// Create a JSON message
		Json::Value message;
		message["action"] = "sendMessage";
		message["body"] = "run_function";

		// Convert the JSON object to a string
		Json::StreamWriterBuilder writer;
		std::string message_str = Json::writeString(writer, message);
		std::cout << message_str << std::endl;
		c.send(hdl_global, message_str, websocketpp::frame::opcode::text);

		// Block main thread until condition_met == true
		{
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait(lock, [] { return condition_met; });
		}

		std::cout << "WE GOOD BABY" << std::endl;



		ReadAudioFileFromS3(bucketName, objectKey);
		std::vector<double> audio_data = read_audio_file("temp_audio.mp3"); // This is for AWS fetched
		if (audio_data.empty()) {
			std::cout << "Time to mono function instead" << std::endl;
			audio_data = read_mono_file("temp_audio.mp3");
			if (audio_data.empty()) {
				std::cout << "stero AND mono functions did not work" << std::endl;
				return 1;
			}
		}
		int n = audio_data.size();
		std::cout << n << std::endl;

		// make analysis output file
		//std::ofstream outputFile("new_fft.txt");
		//std::ofstream magFile("mag.txt");

		int num_chunks = (n + chunk_size - 1) / chunk_size; // calculate all the number of chunks

		applyHanningWindow(audio_data, n);

		// allocate memory for the real input and complex output of this chunk
		int fft_size = chunk_size / 2 + 1; // size of complex output for this chunk
		double* real_input = fftw_alloc_real(chunk_size); // real input array
		if (real_input == nullptr) { // handle allocation error
			std::cerr << "Failed to allocate real memory" << std::endl;
			exit(1);
		}
		fftw_complex* complex_output = fftw_alloc_complex(fft_size); // complex output array
		if (complex_output == nullptr) { // handle allocation error
			std::cerr << "Failed to allocated complex memory" << std::endl;
		}

		// create fftw plan for this chunk
		fftw_plan plan = fftw_plan_dft_r2c_1d(chunk_size, real_input, complex_output, FFTW_MEASURE);

		// create vector for magnitude storage
		std::vector<double> magnitudes(chunk_size);

		int counter = -1;

		// loop over each chunk
		for (int chunk = 0; chunk < num_chunks - 1; ++chunk) { // num_chuks is strange
			++counter;
			std::cout << "chunk: " << chunk << std::endl;
			std::cout << "num chunk " << num_chunks << std::endl;
			//std::cout << "counter: " << counter << std::endl;
			// Determine the starting and ending index for the chunk
			int start = chunk * chunk_size;
			int end = std::min<int>(start + chunk_size, n); // added <int> identifier


			// copy the audio source data into the real_input array
			std::fill(real_input, real_input + chunk_size, 0); // Zero out the input
			std::copy(audio_data.begin() + start, audio_data.begin() + end, real_input);

			//execute the fft
			fftw_execute(plan);

			// open file to store analysis
			//if (outputFile.is_open()) {
					// process the complex output
			for (int i = 0; i < fft_size; ++i) {
				//std::cout << "samples: " << i << " " << audio_data[i + chunk_size] << std::endl;
				double freq = static_cast<double>(i) * sampleRate / chunk_size;
				magnitudes[i] = std::sqrt(complex_output[i][0] * complex_output[i][0]
					+ complex_output[i][1] * complex_output[i][1]);
				// push freq/bins/magnitude to text file
				//outputFile << "bin " << i << ": Frequency = " << freq << " Hz, magnitude = "
					//<< magnitudes[i] << "\n";
			}

			//check magnitudes for wanted frequencies
			isGreaterThanAll(magnitudes, magnitudes[23], counter, audio_data, chunk_size);
			//} // curly for output file

		}
		// I wonder if this is supposed to be in the for loop.
		//outputFile.close();

	/*	if (magFile.is_open()) {
			for (double element : sampleStorage) {
				magFile << "sample: " << element << std::endl;
			}
		}
		magFile.close();*/


		if (sampleStorage.size() >= productDurationSamples) {
			// open wav file for writing
			infiniteFile = sf_open(outputName, SFM_WRITE, &sf_info);
			if (!infiniteFile) {
				std::cerr << "error opening file: " << outputName << std::endl;
				return 1;
			}

			// write data for writing wav file
			sf_count_t framesWritten = sf_writef_double(infiniteFile, sampleStorage.data(), sampleStorage.size()); // might need 'sizeof' for size
			if (framesWritten != sampleStorage.size()) {
				std::cerr << "Error writing data: " << sf_strerror(infiniteFile) << std::endl;
			}

			sf_close(infiniteFile);

			// open the file to upload to s3
			std::ifstream file_stream(outputName, std::ios::in | std::ios::binary);
			if (!file_stream) {
				std::cerr << "Error: Could not open " << outputName << " for s3 upload" << std::endl;
				return 1;
			}

			// Initialize the AWS SDK
			Aws::SDKOptions options;
			Aws::InitAPI(options);

			// Create S3 client
			Aws::Client::ClientConfiguration config;
			config.region = "us-east-2";
			Aws::S3::S3Client s3_client(config);

			// productKey
			std::string productKey = generateTimestampID();

			// Create a putObjectRequest
			Aws::S3::Model::PutObjectRequest object_request;
			object_request.SetBucket(bucketName);
			object_request.SetKey(productKey);

			// Attatch the file stream to the putObject Request
			auto input_data = Aws::MakeShared<Aws::IOStream>("AllocTag", file_stream.rdbuf());
			object_request.SetBody(input_data);

			// Perform the file upload to S3
			auto put_object_outcome = s3_client.PutObject(object_request);

			if (put_object_outcome.IsSuccess()) {
				std::cout << "Product successfully uploaded to S3!" << std::endl;
				Json::Value message;
				message["action"] = "sendMessage";
				message["body"] = "finish_now";

				// Convert the JSON object to a string
				Json::StreamWriterBuilder writer;
				std::string message_str = Json::writeString(writer, message);
				std::cout << message_str << std::endl;
				c.send(hdl_global, message_str, websocketpp::frame::opcode::text);
				task_done = true;
			}
			else {
				std::cerr << "Error uploading product: " << put_object_outcome.GetError().GetMessage() << std::endl;
			}

			Aws::ShutdownAPI(options);

		}

		//clean up
		fftw_destroy_plan(plan);
		fftw_free(real_input);
		fftw_free(complex_output);

		// i think this is how you relock a condition
		std::lock_guard<std::mutex> lock(mtx);
		condition_met = false;

	}

	std::cout << "FINISHED" << std::endl;
}