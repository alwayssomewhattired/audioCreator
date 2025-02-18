#include <iostream>
#include <vector>
#include <sndfile.h>
#include <fftw3.h>
#include <fstream>




//function to read + convert source audio to mono
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






int main()
{
	std::string filename = "C:/Users/zacha/Desktop/empty/500.mp3";
	std::vector<double> audio_data = read_audio_file(filename);
	int n = audio_data.size();

	if (audio_data.empty()) {
		return 1;
	}

	// make analysis output file
	std::ofstream outputFile("new_fft.txt");


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


	// loop over each chunk
	for (int chunk = 0; chunk < num_chunks; ++chunk) {
		std::cout << "chunk: " << chunk << std::endl;
		// Determine the starting and ending index for the chunk
		int start = chunk * chunk_size;
		int end = std::min(start + chunk_size, n);


		// copy the audio source data into the real_input array
		std::fill(real_input, real_input + chunk_size, 0.0); // Zero out the input
		std::copy(audio_data.begin() + start, audio_data.begin() + end, real_input);


		//execute the fft
		fftw_execute(plan);

		// open file to store analysis
		if (outputFile.is_open()) {
			// process the complex output
			for (int i = 0; i < fft_size; ++i) {
				double freq = static_cast<double>(i) * sampleRate / chunk_size;
				magnitudes[i] = std::sqrt(complex_output[i][0] * complex_output[i][0]
					+ complex_output[i][1] * complex_output[i][1]);
				outputFile << "bin " << i << ": Frequency = " << freq << " Hz, magnitude = "
					<< magnitudes[i] << "\n";
			}
		}

	}
	std::cout << "FINISHED" << std::endl;
	//clean up
	fftw_destroy_plan(plan);
	fftw_free(real_input);
	fftw_free(complex_output);
}