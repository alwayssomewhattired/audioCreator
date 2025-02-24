
#define _USE_MATH_DEFINES
// websockets
#include <winsock.h> // This creates naming conflicts
#include <iostream>
#include <vector>
#include <sndfile.h>
#include <fftw3.h>
#include <fstream>
#include <cmath>

// AWS
#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>



// to-do

// I think I'm applying window effect to the actual signal. The resulting audio fades in and out. I should apply the 
// window to a reference to the actual audio instead.

// send fetched python audio to here

// samples clip at times (past -1 or 1). Might be because of uninitialize sampleStorage size.
// initiaized sampleStorage size and still have issue...

// increase amount of bins. 
// you can do this by increasing fft size (increases time it takes).
// You can also change the bins from linear to exponential.

// 5295744 (2 seconds)
// if sample storage exceeds this size, cancel everything and make the wav with it's data.

// add a normalisation feature that compresses the audio.




#pragma comment(lib, "ws2_32.lib") // Link Winsock library

#define PORT 5000
#define SERVER_IP "127.0.0.1"


int websocket()
{
	// Initialize Winsock
	WSADATA wsaData;
	char receivedBuffer[1024] = { 0 };
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "WSAStartup failed. \n";
		return 1;
	}

	// Create socket
	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET) {
		std::cerr << "Socket creation failed.\n";
		WSACleanup();
		return 1;
	}

	// Server address structure
	sockaddr_in server_addr{};
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

	// Connect to server
	if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
		std::cerr << "Connection to server failed.\n";
		closesocket(sock);
		WSACleanup();
		return 1;
	}

	// Send command to Python server
	const char* message = "run_function";
	send(sock, message, strlen(message), 0);

	std::cout << "Command sent to Python server.\n";

	// Receive message
	int bytesReceived = recv(sock, receivedBuffer, sizeof(receivedBuffer) - 1, 0);
	if (bytesReceived > 0) {
		receivedBuffer[bytesReceived] = '\0'; // NuLL-terminate received data
		std::string str(receivedBuffer);
		size_t pos = str.find(":");
		std::string substring = str.substr(pos + 1);
		std::cout << "Received from server: " << receivedBuffer << std::endl;
		std::cout << "Chopped string: " << substring << std::endl;
	}
	else {
		std::cerr << "Failed to receive message\n";
	}

	// Cleanup
	closesocket(sock);
	WSACleanup();

	return 0;

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



// create function to apply a hanning window 
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
		//sampleStorage.emplace_back(samples[i]); // This might be breaking 
		sampleStorage.push_back(samples[i]); // This might fix it
		//std::cout << "stored samples!!!: " << samples[i] << std::endl;
	}
	return true;
}



// vcpkg uses a very long path and might make the aws fail. Move it to a short path ex: "C:\src\vcpkg"

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
	websocket();
	const Aws::String bucketName = "firstdemoby";
	const Aws::String objectKey = "fetch-test.mp3";

	//ReadAudioFileFromS3(bucketName, objectKey);

	std::string filename = "C:/Users/zacha/Desktop/empty/DeathGrips-Exmilitary-1-Beware.mp3";
	std::vector<double> audio_data = read_audio_file(filename);
	//std::vector<double> audio_data = read_audio_file("temp_audio.mp3"); // This is for AWS fetched
	int n = audio_data.size();
	std::cout << n << std::endl;
	if (audio_data.empty()) {
		return 1;
	}

	// make analysis output file
	//std::ofstream outputFile("new_fft.txt");
	std::ofstream magFile("mag.txt");



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

	applyHanningWindow(audio_data, n);

	int num_chunks = (n + chunk_size - 1) / chunk_size; // calculate all the number of chunks


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

	if (magFile.is_open()) {
		for (double element : sampleStorage) {
			magFile << "sample: " << element << std::endl;
		}
	}
	magFile.close();

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

	//clean up
	fftw_destroy_plan(plan);
	fftw_free(real_input);
	fftw_free(complex_output);
	
	std::cout << "FINISHED" << std::endl;
}