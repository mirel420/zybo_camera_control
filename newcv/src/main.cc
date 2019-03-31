#include <opencv2/core.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/objdetect.hpp>

#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/fcntl.h>
#include <fstream>
#include <stdlib.h>
#include <vector>
#include <numeric>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <signal.h>

#define CHARVIDEO_IOC_MAGIC  '8'
#define MOTION_IOC_MAGIC  '9'

#define CHARVIDEO_IOCHALT    _IO(CHARVIDEO_IOC_MAGIC, 0)
#define CHARVIDEO_IOCSTART    _IO(CHARVIDEO_IOC_MAGIC, 1)
#define CHARVIDEO_IOCSTATUS    _IO(CHARVIDEO_IOC_MAGIC, 2)

#define CHARVIDEO_IOCQHEIGHT _IOR(CHARVIDEO_IOC_MAGIC,  3, int)
#define CHARVIDEO_IOCQWIDTH _IOR(CHARVIDEO_IOC_MAGIC,  4, int)
#define CHARVIDEO_IOCQPIXELLEN _IOR(CHARVIDEO_IOC_MAGIC,  5, int)
#define CHARVIDEO_IOCQBUFSIZE _IOR(CHARVIDEO_IOC_MAGIC,  6, int)

#define SERVO_LEFT 220
#define SERVO_RIGHT 380
#define SERVO_CENTER (SERVO_LEFT + (SERVO_RIGHT-SERVO_LEFT)/2)

#define MOTION_IOCTSETENABLE    _IO(MOTION_IOC_MAGIC, 0)
#define MOTION_IOCTSETDIR	_IO(MOTION_IOC_MAGIC, 1)

#define Y_1 560

#define LINE_DISTANCE_1 120
#define LINE_DISTANCE_1_OUT 100
#define LINE_DISTANCE_1_IN 280

#define LEFT_MEAN_1 172
#define LEFT_X_1_1 LEFT_MEAN_1 - LINE_DISTANCE_1_OUT
#define LEFT_X_2_1 LEFT_MEAN_1 + LINE_DISTANCE_1_IN

#define RIGHT_MEAN_1 1100
#define RIGHT_X_1_1 RIGHT_MEAN_1 - LINE_DISTANCE_1_IN
#define RIGHT_X_2_1 RIGHT_MEAN_1 + LINE_DISTANCE_1_OUT

#define Y_2 440

#define LINE_DISTANCE_2 100

#define LEFT_MEAN_2 304
#define LEFT_X_1_2 LEFT_MEAN_2 - LINE_DISTANCE_2
#define LEFT_X_2_2 LEFT_MEAN_2 + LINE_DISTANCE_2

#define RIGHT_MEAN_2 963
#define RIGHT_X_1_2 RIGHT_MEAN_2 - LINE_DISTANCE_2
#define RIGHT_X_2_2 RIGHT_MEAN_2 + LINE_DISTANCE_2

#define Y_3 300

#define LINE_DISTANCE_3 100

#define LEFT_MEAN_3 481
#define LEFT_X_1_3 LEFT_MEAN_3 - LINE_DISTANCE_3
#define LEFT_X_2_3 LEFT_MEAN_3 + LINE_DISTANCE_3

#define RIGHT_MEAN_3 794
#define RIGHT_X_1_3 RIGHT_MEAN_3 - LINE_DISTANCE_3
#define RIGHT_X_2_3 RIGHT_MEAN_3 + LINE_DISTANCE_3

#define SIGN_MIN 150
#define SIGN_MAX 200

#define CLK_FREQ 50000000.0f // FCLK0 frequency not found in xparameters.h
const double clk_to_cm = (((1000000.0f / CLK_FREQ) * 2.54f) / 147.0f);

int f_motors;
int f_servo;

using namespace cv;
using namespace std;

CascadeClassifier stop_cascade;
String stop_cascade_name = "stop.xml";

int full_map(double input, double in_min, double in_max, double out_min, double out_max) {

	double slope = (double) (out_max - out_min) / (double) (in_max - in_min);
	double output = (double) out_min + slope * (double) (input - in_min);
	return (int) output;
}

int map_servo(double input, double in_min, double in_max) {

	return full_map(input, in_min, in_max, SERVO_LEFT, SERVO_RIGHT);

}

int map_servo_fine(double input, double in_min, double in_max, double mean) {

	if (input < mean) {
		return full_map(input, in_min, mean, SERVO_LEFT, SERVO_CENTER);
	} else {
		return full_map(input, mean, in_max, SERVO_CENTER, SERVO_RIGHT);
	}

}

double average_not_zero(int a, int b) {

	if (a != 0 && b != 0) {
		return (double) (a + b) / 2;
	} else if (a != 0) {
		return a;
	} else if (b != 0) {
		return b;
	} else {
		return -1;
	}
}

int chose_servo(int left, int right, int mean) {
	if (left != 0 && right != 0) {
		if (abs(right - left) < 15) {
			return average_not_zero(left, right);
		} else {
			if (left < mean && right < mean) {
				return min(left, right);
			} else if (left > mean && right > mean) {
				return max(left, right);
			} else {
				return average_not_zero(left, right);
			}
		}
	} else {
		return average_not_zero(left, right);
	}
}

double find_avg_point_on_line(Mat frame, Mat image, int line_y, int line_start, int line_stop, int param) {

	std::vector<int> v;
	double ret = -1;
	for (int i = 0; i < line_stop - line_start + 3; i += 1) {
		if (frame.at<uchar>(Point(i + line_start, line_y)) == 255) {
			if (param > 0 || param == -1) {
				cout << "px " << i + line_start << endl;
			}
			if (param > 1 || param == -1) {
				line(image, Point(i + line_start, line_y - 7), Point(i + line_start, line_y + 7), Scalar(0, 0, 255), 4, CV_AA);
			}
			v.push_back(i + line_start);
		}
	}

	if (v.size() > 0) {
		ret = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
		line(image, Point(ret, line_y - 10), Point(ret, line_y + 10), Scalar(255, 0, 255), 3, CV_AA);
	}
	return ret;
}

int servo_comand_line(Mat frame, Mat image, int param) {

	int w = frame.cols;
	int h = frame.rows;

	double left_avg_1, right_avg_1, left_avg_2, right_avg_2, left_avg_3, right_avg_3;

	if (param > 0 || param == -1) {
		cout << "left1++" << endl;
	}
	left_avg_1 = find_avg_point_on_line(frame, image, Y_1, LEFT_X_1_1, LEFT_X_2_1, param);

	if (param > 0 || param == -1) {
		cout << "right1++" << endl;
	}
	right_avg_1 = find_avg_point_on_line(frame, image, Y_1, RIGHT_X_1_1, RIGHT_X_2_1, param);

	if (param > 0 || param == -1) {
		cout << "left2++" << endl;
	}
	left_avg_2 = find_avg_point_on_line(frame, image, Y_2, LEFT_X_1_2, LEFT_X_2_2, param);

	if (param > 0 || param == -1) {
		cout << "right2++" << endl;
	}
	right_avg_2 = find_avg_point_on_line(frame, image, Y_2, RIGHT_X_1_2, RIGHT_X_2_2, param);

	if (param > 0 || param == -1) {
		cout << "left3++" << endl;
	}
	left_avg_3 = find_avg_point_on_line(frame, image, Y_3, LEFT_X_1_3, LEFT_X_2_3, param);

	if (param > 0 || param == -1) {
		cout << "right3++" << endl;
	}
	right_avg_3 = find_avg_point_on_line(frame, image, Y_3, RIGHT_X_1_3, RIGHT_X_2_3, param);

	int servo_left1 = 0, servo_right1 = 0, servo_left2 = 0, servo_right2 = 0, servo_left3 = 0, servo_right3 = 0;

	if (left_avg_1 != -1) {
		servo_left1 = map_servo_fine(left_avg_1, LEFT_X_1_1, LEFT_X_2_1, LEFT_MEAN_1);
		if (left_avg_2 != -1) {
			servo_left2 = map_servo_fine(left_avg_2, LEFT_X_1_2, LEFT_X_2_2, LEFT_MEAN_2);
			if (left_avg_3 != -1) {
				servo_left3 = map_servo_fine(left_avg_3, LEFT_X_1_3, LEFT_X_2_3, LEFT_MEAN_3);
			}
		}
	}

	if (right_avg_1 != -1) {
		servo_right1 = map_servo_fine(right_avg_1, RIGHT_X_1_1, RIGHT_X_2_1, RIGHT_MEAN_1);
		if (right_avg_2 != -1) {
			servo_right2 = map_servo_fine(right_avg_2, RIGHT_X_1_2, RIGHT_X_2_2, RIGHT_MEAN_2);
			if (right_avg_3 != -1) {
				servo_right3 = map_servo_fine(right_avg_3, RIGHT_X_1_3, RIGHT_X_2_3, RIGHT_MEAN_3);
			}
		}
	}

	if (param > 0 || param == -1) {
		cout << "lft  = " << servo_left1 << ", rgt = " << servo_right1 << endl;
	}

	return chose_servo(servo_left1, servo_right1, SERVO_CENTER);
}

void one_poly_sign(Mat img) {
	int w = img.cols;
	int h = img.rows;

	int x1, x2, y1, y2;
	x1 = (int) w / 2;
	y1 = 0;
	x2 = w;
	y2 = h;

	int lineType = 8;
	/* Create some points */
	Point pts[1][4];
	pts[0][0] = Point(x1, y1);
	pts[0][1] = Point(x1, y2);
	pts[0][2] = Point(x2, y2);
	pts[0][3] = Point(x2, y1);

	const Point* ppt[1] = { pts[0] };
	int npt[] = { 4 };
	fillPoly(img, ppt, npt, 1, Scalar(255, 255, 255), lineType);
}

Mat crop(Mat frame, int x1, int y1, int x2, int y2) {

	Rect roi;
	roi.x = x1;
	roi.y = y1;
	roi.width = x2 - x1;
	roi.height = y2 - y1;

	Mat crop = frame(roi);
	return crop;
}

void lines(Mat img) {
// the 1st selection lines
	line(img, Point(RIGHT_X_1_1, Y_1), Point(RIGHT_X_2_1, Y_1), Scalar(255, 255, 255), 3, CV_AA);
	line(img, Point(LEFT_X_1_1, Y_1), Point(LEFT_X_2_1, Y_1), Scalar(255, 255, 255), 3, CV_AA);
// the 2nd selection lines
	line(img, Point(RIGHT_X_1_2, Y_2), Point(RIGHT_X_2_2, Y_2), Scalar(255, 255, 255), 3, CV_AA);
	line(img, Point(LEFT_X_1_2, Y_2), Point(LEFT_X_2_2, Y_2), Scalar(255, 255, 255), 3, CV_AA);
// the 3rd selection lines
	line(img, Point(RIGHT_X_1_3, Y_3), Point(RIGHT_X_2_3, Y_3), Scalar(255, 255, 255), 3, CV_AA);
	line(img, Point(LEFT_X_1_3, Y_3), Point(LEFT_X_2_3, Y_3), Scalar(255, 255, 255), 3, CV_AA);
}

int detect_and_display(Mat frame, Mat image, int param) {

	int ret = 0;
	std::vector<Rect> stop_signs;
	Mat frame_gray;
	cvtColor(frame, frame_gray, COLOR_BGR2GRAY);
	equalizeHist(frame_gray, frame_gray);
//-- Detect stop_signs
	stop_cascade.detectMultiScale(frame_gray, stop_signs, 1.1, 2, 0 | CASCADE_SCALE_IMAGE, Size(30, 30));
	if (param > 0 || param == -1) {
		cout << "stop signs = " << stop_signs.size() << endl;
	}

	for (size_t i = 0; i < stop_signs.size(); i++) {
		if (param > 0 || param == -1) {
			cout << "stop " << i << " height = " << stop_signs[i].height << endl;
			cout << "stop " << i << " width = " << stop_signs[i].width << endl;

			// draw a ellipse around the sign
			if (param > 2 || param == -1) {
				Point center(stop_signs[i].x + stop_signs[i].width / 2, stop_signs[i].y + stop_signs[i].height / 2);
				ellipse(frame, center, Size(stop_signs[i].width / 2, stop_signs[i].height / 2), 0, 0, 360, Scalar(255, 0, 255), 4, 8, 0);
			}
		}
		if (stop_signs[i].height >= SIGN_MIN && stop_signs[i].height <= SIGN_MAX) {
			ret = 1;
		}
	}
	return ret;

}

void my_handler(int s) {

	unsigned int speed = 0;
	int servo_out = SERVO_CENTER;

	write(f_servo, &servo_out, 2);
	write(f_motors, &speed, 4);

	exit(1);
}

int main(int argc, char** argv) {

	cout << "OpenCV version : " << CV_VERSION << endl;

	int param = 0;

	if (argc < 5) {
		cerr << "./opencvcarctrl.elf <debug param> <number of iterations> <speed> <direction> <stop distance>" << endl << "0 - nothing" << endl << "1 - just text" << endl
				<< "2 - one *relevant* image" << endl << "3 - more *relevant* images" << endl << "-1 - full debug mode test.png used" << endl;
		return -1;
	}

	param = atoi(argv[1]);

	FILE* camera = fopen("/dev/video", "rb");
	if (camera < 0) {
		cerr << "Failed to open camera." << endl;
		return -1;
	}

	FILE* servo = fopen("/dev/servo", "r+b");
	if (servo < 0) {
		cerr << "Failed to open servo." << endl;
		fclose(camera);
		return -1;
	}

	FILE* motors = fopen("/dev/motors", "r+b");
	if (motors < 0) {
		cerr << "Failed to open motors." << endl;
		fclose(camera);
		fclose(servo);
		return -1;
	}

	FILE* sonar = fopen("/dev/sonar", "rb");
	if (argc > 5) {
		if (sonar < 0) {
			cerr << "Failed to open sonar." << endl;
			fclose(camera);
			fclose(servo);
			fclose(motors);
			return -1;
		}
	}

	f_motors = motors->_fileno;
	f_servo = servo->_fileno;

	int iterations = atoi(argv[2]);
	if (iterations < 0) {
		cerr << "Bad number of iterations." << endl;
		fclose(camera);
		fclose(servo);
		fclose(motors);
		fclose(sonar);
		return -1;
	} else if (iterations == 42) {
		iterations = 10000;
	}
	if (param == -1) {
		iterations = 1;
	}

	unsigned short left_speed = atoi(argv[3]);
	unsigned short right_speed = left_speed;
	unsigned int speed = (left_speed << 16) + right_speed;

	unsigned int left_dir = atoi(argv[4]);
	if (left_dir < 0 && left_dir > 1) {
		cerr << "Bad direction." << endl;
		fclose(camera);
		fclose(servo);
		fclose(motors);
		fclose(sonar);
		return -1;
	}
	unsigned int right_dir = left_dir;
	ioctl(motors->_fileno, MOTION_IOCTSETDIR, ((left_dir & 1) << 1) + (right_dir & 1));

	unsigned int enable = 1;
	ioctl(motors->_fileno, MOTION_IOCTSETENABLE, enable);

	int servo_out = 300;
	int old_servo_out = servo_out;

	int srv_write = write(servo->_fileno, &servo_out, 2);
	//int mtr_write = write(motors->_fileno, &speed, 4);
	int mtr_write;

	unsigned int clk_edges;
	double dist;
	double stop_dist = atoi(argv[5]);

	int stop_sign = 0, stop_sign_iteration = 0, old_stop_sign = 0;

	if (!stop_cascade.load(stop_cascade_name)) {
		cerr << "--(!)Error loading stop cascade" << endl;
		fclose(camera);
		fclose(servo);
		fclose(motors);
		if (argc > 5) {
			fclose(sonar);
		}
		return -1;
	};

	struct sigaction sigIntHandler;

	memset(&sigIntHandler, 0, sizeof(sigIntHandler));

	sigIntHandler.sa_flags = SA_RESETHAND;
	sigIntHandler.sa_handler = my_handler;

	sigaction(SIGINT, &sigIntHandler, NULL);

	unsigned char* pixels;
	int h, w, l;
	h = ioctl(camera->_fileno, CHARVIDEO_IOCQHEIGHT);
	w = ioctl(camera->_fileno, CHARVIDEO_IOCQWIDTH);
	l = ioctl(camera->_fileno, CHARVIDEO_IOCQPIXELLEN);
	cout << h << endl << w << endl << l << endl;
	pixels = (unsigned char *) malloc(h * w * l * sizeof(char));

	for (int loop = 0; loop < iterations; loop++) {

		if (atoi(argv[1]) > 0) {
			cout << "loop=====================" << loop << endl;

		}

		fread(pixels, 1, h * w * l, camera);

		Mat image;

		if (loop == 0 && param == -1) {
			image = imread("test.png", CV_LOAD_IMAGE_COLOR);
		} else {
			image = Mat(h, w, CV_8UC3, &pixels[0]);
		}

		Mat frame;
		image.copyTo(frame);

		Mat gray_image;
		cvtColor(frame, gray_image, CV_RGB2GRAY);

		Mat blurred_image;
		GaussianBlur(gray_image, blurred_image, Size(5, 5), 0, 0);

		Mat canny_image;
		Canny(blurred_image, canny_image, 50, 150);

		// make the selection areas for the images
		Mat poly;
		poly = cv::Mat::zeros(frame.size(), canny_image.type());
		lines(poly);

		// apply selection areas to said images
		Mat region_image_lane, region_image_sign_1, region_image_sign_2;
		bitwise_and(poly, canny_image, region_image_lane);
		//image.copyTo(region_image_sign, poly2);
		region_image_sign_1 = crop(frame, w / 2, 0, w, h);
		cv::resize(region_image_sign_1, region_image_sign_2, cv::Size(), 0.5, 0.5);


		servo_out = servo_comand_line(region_image_lane, image, param);

		if (argc > 5) {
//			read(sonar->_fileno, &clk_edges, 4);
//			dist = clk_edges * clk_to_cm;
//			if (dist < stop_dist) {
//				speed = 0;
//			} else {
//				speed = (left_speed << 16) + right_speed;
//			}
		}

		if (servo_out == -1) {
			servo_out = old_servo_out;
		}

		if (servo_out <= SERVO_LEFT)
			servo_out = SERVO_LEFT;
		if (servo_out >= SERVO_RIGHT)
			servo_out = SERVO_RIGHT;

		old_servo_out = servo_out;

		old_stop_sign = stop_sign;

		stop_sign = detect_and_display(region_image_sign_2, image, param);
//
//		if (stop_sign == 1 && old_stop_sign == 0) {
//			speed = 0;
//			stop_sign_iteration = loop;
//			if (param > 0 || param == -1) {
//				cout << "stop_sign -> STOP NOW!" << endl;
//			}
//		}
//
//		if (old_stop_sign == 1 && stop_sign_iteration == loop - 5) {
//			speed = (left_speed << 16) + right_speed;
//			if (param > 0 || param == -1) {
//				cout << "stop_sign -> GO NOW!" << endl;
//			}
//		}

		mtr_write = write(motors->_fileno, &speed, 4);
		srv_write = write(servo->_fileno, &servo_out, 2);

		if (param > 0 || param == -1) {

			cout << "servo = " << servo_out << endl;
			cout << "speed = " << speed << endl;
			cout << "dist  = " << dist << endl;

			//cout << "Servo write: " << srv_write << endl;
			//cout << "Motor write: " << mtr_write << endl;
		}
		if (param > 1 || param == -1) {
			line(image, Point(RIGHT_X_1_1, Y_1), Point(RIGHT_X_2_1, Y_1), Scalar(100, 0, 0), 2, CV_AA);
			line(image, Point(LEFT_X_1_1, Y_1), Point(LEFT_X_2_1, Y_1), Scalar(0, 100, 0), 2, CV_AA);
			line(image, Point(RIGHT_X_1_2, Y_2), Point(RIGHT_X_2_2, Y_2), Scalar(100, 0, 0), 2, CV_AA);
			line(image, Point(LEFT_X_1_2, Y_2), Point(LEFT_X_2_2, Y_2), Scalar(0, 100, 0), 2, CV_AA);
			line(image, Point(RIGHT_X_1_3, Y_3), Point(RIGHT_X_2_3, Y_3), Scalar(100, 0, 0), 2, CV_AA);
			line(image, Point(LEFT_X_1_3, Y_3), Point(LEFT_X_2_3, Y_3), Scalar(0, 100, 0), 2, CV_AA);

			line(image, Point(LEFT_MEAN_1, Y_1 - 5), Point(LEFT_MEAN_1, Y_1 + 5), Scalar(0, 255, 255), 2, CV_AA);
			line(image, Point(RIGHT_MEAN_1, Y_1 - 5), Point(RIGHT_MEAN_1, Y_1 + 5), Scalar(0, 255, 255), 2, CV_AA);
			line(image, Point(LEFT_MEAN_2, Y_2 - 5), Point(LEFT_MEAN_2, Y_2 + 5), Scalar(0, 255, 255), 2, CV_AA);
			line(image, Point(RIGHT_MEAN_2, Y_2 - 5), Point(RIGHT_MEAN_2, Y_2 + 5), Scalar(0, 255, 255), 2, CV_AA);
			line(image, Point(LEFT_MEAN_3, Y_3 - 5), Point(LEFT_MEAN_3, Y_3 + 5), Scalar(0, 255, 255), 2, CV_AA);
			line(image, Point(RIGHT_MEAN_3, Y_3 - 5), Point(RIGHT_MEAN_3, Y_3 + 5), Scalar(0, 255, 255), 2, CV_AA);

			vector<int> compression_params;
			compression_params.push_back(CV_IMWRITE_PNG_COMPRESSION);
			compression_params.push_back(9);

			char name[20];
			sprintf(name, "img%d.png", loop);
			try {

				imwrite(name, image, compression_params);
				imwrite("test2.png", frame, compression_params);

				if (param > 2 || param == -1) {
					imwrite("img_region_image_lane.png", region_image_lane, compression_params);
					imwrite("img_region_image_sign.png", region_image_sign_2, compression_params);
				}
				if (param == -1) {
					imwrite("img_poly.png", poly, compression_params);
				}

			} catch (runtime_error& ex) {
				cerr << "Exception converting image to PNG format: " << ex.what() << endl;
				return 1;
			}
		}

		sigaction(SIGINT, &sigIntHandler, NULL);

	}

	speed = 0;
	servo_out = 300;

	srv_write = write(servo->_fileno, &servo_out, 2);
	mtr_write = write(motors->_fileno, &speed, 4);
	if (param > 0) {
		cout << "Servo write: " << srv_write << endl;
		cout << "Motor write: " << mtr_write << endl;
	}

	fclose(camera);
	fclose(servo);
	fclose(motors);

	return 0;
}