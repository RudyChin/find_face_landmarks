#include "sfl/sequence_face_landmarks.h"

#ifdef WITH_PROTOBUF
#include "sequence_face_landmarks.pb.h"
#endif // WITH_PROTOBUF

// std
#include <exception>

// OpenCV
#include <opencv2/imgproc.hpp>

// dlib
#include <dlib/opencv.h>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_processing/render_face_detections.h>
#include <dlib/image_processing/shape_predictor.h>

using std::string;
using std::runtime_error;

namespace sfl
{
	class SequenceFaceLandmarksImpl : public SequenceFaceLandmarks
	{
	public:
		SequenceFaceLandmarksImpl(const std::string& modelPath, float frame_scale) :
			m_frame_scale(frame_scale)
		{
			setModel(modelPath);
		}

		SequenceFaceLandmarksImpl(float frame_scale) :
			m_frame_scale(frame_scale) {}

		const Frame& addFrame(const cv::Mat& frame)
		{
			if (m_model_path.empty()) 
				throw runtime_error("A landmarks model file is not set!");

			// Extract landmarks by number of channels
			m_frames.push_back({ {}, frame.cols, frame.rows });
			if (frame.channels() == 3)  // BGR
				extract_landmarks<dlib::bgr_pixel>(frame, m_frames.back());
			else // grayscale
				extract_landmarks<unsigned char>(frame, m_frames.back());

			return m_frames.back();
		}

		const std::vector<Frame>& getSequence() const { return m_frames; }

		void clear()
		{
			m_frames.clear();
		}

#ifdef WITH_PROTOBUF
		void load(const std::string& filePath)
		{
			clear();

			// Read from file
			io::Sequence sequence;
			std::ifstream input(filePath, std::ifstream::binary);
			sequence.ParseFromIstream(&input);

			// Convert format
			m_frames.reserve(sequence.frames_size());

			// For each frame in the sequence
			for (const io::Frame& io_frame : sequence.frames())
			{
				Frame frame;
				frame.width = io_frame.width();
				frame.height = io_frame.height();
				frame.faces.reserve(io_frame.faces_size());

				// For each face detected in the frame
				for (const io::Face& io_face : io_frame.faces())
				{
					Face face;
					const io::BoundingBox& io_bbox = io_face.bbox();
					face.bbox.x = io_bbox.left();
					face.bbox.y = io_bbox.top();
					face.bbox.width = io_bbox.width();
					face.bbox.height = io_bbox.height();
					face.landmarks.reserve(io_face.landmarks_size());

					// For each landmark point in the face
					for (const io::Point& io_point : io_face.landmarks())
						face.landmarks.push_back(cv::Point(io_point.x(), io_point.y()));

					frame.faces.push_back(face);
				}

				m_frames.push_back(frame);
			}
		}

		void save(const std::string& filePath) const
		{
			// Convert format
			io::Sequence sequence;

			// For each frame in the sequence
			for (const Frame& frame : m_frames)
			{
				io::Frame* io_frame = sequence.add_frames();
				io_frame->set_width(frame.width);
				io_frame->set_height(frame.height);

				// For each face detected in the frame
				for (const Face& face : frame.faces)
				{
					io::Face* io_face = io_frame->add_faces();
					io::BoundingBox* io_bbox = io_face->mutable_bbox();
					io_bbox->set_left(face.bbox.x);
					io_bbox->set_top(face.bbox.y);
					io_bbox->set_width(face.bbox.width);
					io_bbox->set_height(face.bbox.height);

					// For each landmark point in the face
					for (const cv::Point& point : face.landmarks)
					{
						io::Point* io_point = io_face->add_landmarks();
						io_point->set_x(point.x);
						io_point->set_y(point.y);
					}
				}
			}

			// Write to file
			std::ofstream output(filePath, std::fstream::trunc | std::fstream::binary);
			sequence.SerializeToOstream(&output);
		}
#else
		const std::string NO_PROTOBUF_ERROR =
			"Method is not implemented! Please enable protobuf to use.";
		void load(const std::string& filePath) { throw runtime_error(NO_PROTOBUF_ERROR); }
		void save(const std::string& filePath) const { throw runtime_error(NO_PROTOBUF_ERROR); }
#endif // WITH_PROTOBUF

		void setModel(const std::string& modelPath)
		{
			if (modelPath.empty()) return;
			m_model_path = modelPath;

			// Face detector for finding bounding boxes for each face in an image
			m_detector = dlib::get_frontal_face_detector();

			// Shape predictor for finding landmark positions given an image and face bounding box.
			dlib::deserialize(modelPath) >> m_pose_model;
		}

		size_t size() const { return m_frames.size(); }

		const Frame& operator[](size_t i) const { return m_frames[i]; }

	private:
		template<typename pixel_type>
		void extract_landmarks(const cv::Mat& frame, Frame& frame_landmarks)
		{
			// Scaling
			cv::Mat frame_scaled;
			if (m_frame_scale != 1.0f)
				cv::resize(frame, frame_scaled, cv::Size(),
					m_frame_scale, m_frame_scale);
			else frame_scaled = frame;

			// Convert OpenCV's mat to dlib format 
			dlib::cv_image<pixel_type> dlib_frame(frame_scaled);

			// Detect bounding boxes around all the faces in the image.
			std::vector<dlib::rectangle> faces = m_detector(dlib_frame);

			// Find the pose of each face we detected.
			std::vector<dlib::full_object_detection> shapes;
			frame_landmarks.faces.resize(faces.size());
			for (size_t i = 0; i < faces.size(); ++i)
			{
				Face& face = frame_landmarks.faces[i];
				dlib::rectangle& dlib_face = faces[i];
				dlib::full_object_detection shape = m_pose_model(dlib_frame, dlib_face);
				dlib_obj_to_points(shape, face.landmarks);

				// Scale landmarks to the original frame's pixel coordinates
				for (size_t j = 0; j < face.landmarks.size(); ++j)
				{
					face.landmarks[j].x = (int)std::round(face.landmarks[j].x / m_frame_scale);
					face.landmarks[j].y = (int)std::round(face.landmarks[j].y / m_frame_scale);
				}

				// Set face bounding box
				face.bbox.x = (int)std::round(faces[i].left() / m_frame_scale);
				face.bbox.y = (int)std::round(faces[i].top() / m_frame_scale);
				face.bbox.width = (int)std::round(faces[i].width() / m_frame_scale);
				face.bbox.height = (int)std::round(faces[i].height() / m_frame_scale);
			}
		}

		void dlib_obj_to_points(const dlib::full_object_detection& obj,
			std::vector<cv::Point>& points)
		{
			points.resize(obj.num_parts());
			for (unsigned long i = 0; i < obj.num_parts(); ++i)
			{
				cv::Point& p = points[i];
				const dlib::point& obj_p = obj.part(i);
				p.x = (float)obj_p.x();
				p.y = (float)obj_p.y();
			}
		}

	protected:
		std::vector<Frame> m_frames;
		float m_frame_scale;
		std::string m_model_path;

		// dlib
		dlib::frontal_face_detector m_detector;
		dlib::shape_predictor m_pose_model;
	};

	std::shared_ptr<SequenceFaceLandmarks> SequenceFaceLandmarks::create(
		const std::string& modelPath, float frame_scale)
	{
		return std::make_shared<SequenceFaceLandmarksImpl>(modelPath, frame_scale);
	}

	std::shared_ptr<SequenceFaceLandmarks> SequenceFaceLandmarks::create(
		float frame_scale)
	{
		return std::make_shared<SequenceFaceLandmarksImpl>(frame_scale);
	}

	void render(cv::Mat & img, const std::vector<cv::Point>& landmarks,
		bool drawLabels, const cv::Scalar & color, int thickness)
	{
		if (landmarks.size() == 68)
		{
			for (size_t i = 1; i <= 16; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);

			for (size_t i = 28; i <= 30; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);

			for (size_t i = 18; i <= 21; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);

			for (size_t i = 23; i <= 26; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);

			for (size_t i = 31; i <= 35; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);
			cv::line(img, landmarks[30], landmarks[35], color, thickness);

			for (size_t i = 37; i <= 41; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);
			cv::line(img, landmarks[36], landmarks[41], color, thickness);

			for (size_t i = 43; i <= 47; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);
			cv::line(img, landmarks[42], landmarks[47], color, thickness);

			for (size_t i = 49; i <= 59; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);
			cv::line(img, landmarks[48], landmarks[59], color, thickness);

			for (size_t i = 61; i <= 67; ++i)
				cv::line(img, landmarks[i], landmarks[i - 1], color, thickness);
			cv::line(img, landmarks[60], landmarks[67], color, thickness);	
		}
		else
		{
			for (size_t i = 0; i < 68; ++i)
				cv::circle(img, landmarks[i], thickness, color, -1);
		}

		if (drawLabels)
		{
			// Add labels
			for (size_t i = 0; i < landmarks.size(); ++i)
				cv::putText(img, std::to_string(i), landmarks[i],
					cv::FONT_HERSHEY_PLAIN, 0.5, color, thickness);
		}
	}

	void render(cv::Mat& img, const cv::Rect& bbox, const cv::Scalar& color,
		int thickness)
	{
		cv::rectangle(img, bbox, color, thickness);
	}

	void render(cv::Mat& img, const Face& face, bool drawLabels,
		const cv::Scalar& bbox_color, const cv::Scalar& landmarks_color, int thickness)
	{
		render(img, face.bbox, bbox_color, thickness);
		render(img, face.landmarks, drawLabels, landmarks_color, thickness);
	}

	void render(cv::Mat& img, const Frame& frame, bool drawLabels,
		const cv::Scalar& bbox_color, const cv::Scalar& landmarks_color, int thickness)
	{
		for (const Face& face : frame.faces)
			render(img, face, drawLabels, bbox_color, landmarks_color, thickness);
	}

}   // namespace sfl
