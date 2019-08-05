#include "OpenGLWindow.h"
#include <QOpenGLShaderProgram>

#include <QPainter>

#include <iostream>

OpenGLWindow::OpenGLWindow(DicomManager *dicom_manager, QWindow* parent = 0)
	: QWindow(parent)
{
	this->m_dicom_manager = dicom_manager;

	setSurfaceType(QWindow::OpenGLSurface);
	create();

	// Create an OpenGL context
	m_context = new QOpenGLContext();
	m_context->setFormat(requestedFormat());
	m_context->create();

	// Initialize for OpenGL
	_initializeGL();

	// Connect a signal to listen changing volume data
	connect(m_dicom_manager, &DicomManager::changeVolume, [this] {
		this->loadObject();
		this->render();
	});

	m_is_right_pressed = false;
	// Create ArcBall
	QSize size = this->size();
	arc = new ArcBall(size.width(), size.height());
}

void OpenGLWindow::_initializeGL() {
	qDebug("Initialze gl");
	m_context->makeCurrent(this);
	initializeOpenGLFunctions();

	/* OpenGL information */
	printf("Renderer: %s\n", glGetString(GL_RENDERER));
	printf("OpenGL version: %s\n", glGetString(GL_VERSION));

	GLuint cube_vbo;
	glGenBuffers(1, &cube_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, cube_vbo);
	glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(GLfloat), m_cube_vertices, GL_STATIC_DRAW);

	GLuint cube_ebo;
	glGenBuffers(1, &cube_ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cube_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, 36 * sizeof(GLuint), m_cube_triangle_indices, GL_STATIC_DRAW);

	glGenVertexArrays(1, &m_vao);
	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, cube_vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cube_ebo);
	glEnableVertexAttribArray(0);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Open our shaders

	// Firstpass shader: create 2d texture which saves endpoint of ray.
	m_raycast_firstpass_shader = new QOpenGLShaderProgram;
	m_raycast_firstpass_shader->addShaderFromSourceFile(QOpenGLShader::Vertex, "./Shaders/firstpass.vert");
	m_raycast_firstpass_shader->addShaderFromSourceFile(QOpenGLShader::Fragment, "./Shaders/firstpass.frag");
	m_raycast_firstpass_shader->link();
	
	// Secondpass shader: Calculate color with ray
	m_raycast_shader = new QOpenGLShaderProgram();
	m_raycast_shader->addShaderFromSourceFile(QOpenGLShader::Vertex, "./Shaders/raycast.vert");
	m_raycast_shader->addShaderFromSourceFile(QOpenGLShader::Fragment, "./Shaders/raycast.frag");
	m_raycast_shader->link();

	QSize size = this->size();
	_initializeTargetTexture(size.width(), size.height());
	_initializeFramebuffer(size.width(), size.height());

	_initializeMatrix();

	glInitialized = true;
}

/**
 * Reset all matrix and re-rendering
 */
void OpenGLWindow::reset() {
	_initializeMatrix();
	render();
}

void OpenGLWindow::_initializeMatrix() {

	// Rotate to show sagittal view at first
	m_model_mat.rotate(180.0, 0.0, 0.0, 1.0);
	m_model_mat.rotate(-90.0, 1.0, 0.0, 0.0);

	// Translate model to center of screen
	m_model_mat.translate(-0.5, -0.5, -0.5);

	m_view_mat.lookAt(
		QVector3D(0.0, 0.0, 1.0),
		QVector3D(0.0, 0.0, 0.0),
		QVector3D(0.0, 1.0, 0.0));
}

void OpenGLWindow::loadObject() {

	// Load Volume data
	_loadVolume();

	volumeload = true;
}

void OpenGLWindow::_loadVolume() {

	glDeleteTextures(1, &m_volume_texture);
	glGenTextures(1, &m_volume_texture);
	glEnable(GL_TEXTURE_3D);

	glBindTexture(GL_TEXTURE_3D, m_volume_texture); // Bind
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	int width = std::get<0>(m_dicom_manager->getVolumeSize());
	int height = std::get<1>(m_dicom_manager->getVolumeSize());
	int depth = std::get<2>(m_dicom_manager->getVolumeSize());

	glTexImage3D(GL_TEXTURE_3D, 0, GL_R16F,
		width, height, depth, 0, GL_RED, GL_FLOAT, (m_dicom_manager->getVolumeBuffer()));

	GLenum err;
	while ((err = glGetError()) != GL_NO_ERROR) {
		std::cerr << "OpenGL error: " << err << std::endl;
	}

	glBindTexture(GL_TEXTURE_3D, 0); // Release
}

/**
 * Display the volume on our screen.
 */
void OpenGLWindow::render()
{
	m_context->makeCurrent(this);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	if (volumeload) {
		QSize size = this->size();

		qDebug("painting");

		/* First Pass */
		glEnable(GL_DEPTH_TEST);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_framebuffer);
		glViewport(0, 0, size.width(), size.height());

		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		m_raycast_firstpass_shader->bind();
		_renderCube(m_raycast_firstpass_shader, GL_FRONT);
		m_raycast_firstpass_shader->release();

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		
		/* Second Pass */
		// volume data
		m_raycast_shader->bind();
		glViewport(0, 0, size.width(), size.height());

		glEnable(GL_DEPTH_TEST);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// first-pass result which is filled with position data
		glActiveTexture(GL_TEXTURE0);
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, m_target_texture);
		glUniform1i(glGetUniformLocation(m_raycast_shader->programId(), "backtex"), 0);

		glActiveTexture(GL_TEXTURE1);
		glEnable(GL_TEXTURE_3D);
		glBindTexture(GL_TEXTURE_3D, m_volume_texture);
		glUniform1i(glGetUniformLocation(m_raycast_shader->programId(), "voltex"), 1);

		glUniform1f(glGetUniformLocation(m_raycast_shader->programId(), "screen_width"), (GLfloat) this->size().width());
		glUniform1f(glGetUniformLocation(m_raycast_shader->programId(), "screen_height"), (GLfloat) this->size().height());

		_renderCube(m_raycast_shader, GL_BACK);

		m_raycast_shader->release();
	}

	m_context->swapBuffers(this);
}

void OpenGLWindow::_renderCube(QOpenGLShaderProgram* shader, GLuint cull_face) {
	glEnable(GL_DEPTH_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	
	QMatrix4x4 proj;
	//proj.perspective(60.0, 1.0, 0.1f, 100.0f);
	proj.ortho(-0.7f, 0.7f, -0.7f, 0.7f, 0.1f, 100.0f);
	QMatrix4x4 rotated_view = m_view_mat * QMatrix4x4(arc->getRotationMatrix());
	QMatrix4x4 mvp = proj * rotated_view * m_model_mat;
	glUniformMatrix4fv(glGetUniformLocation(shader->programId(), "mvp"), 1, GL_FALSE, mvp.constData());

	glEnable(GL_CULL_FACE);
	glCullFace(cull_face);
	glBindVertexArray(m_vao);
	glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
	glDisable(GL_CULL_FACE);
}

void OpenGLWindow::_initializeTargetTexture(int width, int height) {
	glDeleteTextures(1, &m_target_texture);
	glGenTextures(1, &m_target_texture);
	glBindTexture(GL_TEXTURE_2D, m_target_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);

	glBindTexture(GL_TEXTURE_2D, 0); // release
}

void OpenGLWindow::_initializeFramebuffer(int width, int height) {
	glDeleteRenderbuffers(1, &m_depth_buffer);
	glDeleteFramebuffers(1, &m_framebuffer);

	// depth buffer for face culling
	glGenRenderbuffers(1, &m_depth_buffer);
	glBindRenderbuffer(GL_RENDERBUFFER, m_depth_buffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);

	// framebuffer for rendering 
	glGenFramebuffers(1, &m_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_target_texture, 0);

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth_buffer);

	glBindFramebuffer(GL_FRAMEBUFFER, 0); // release
}


/* Qt Mouse Event */
void OpenGLWindow::mouseMoveEvent(QMouseEvent* e) {
	QPoint cur_point = e->pos();

	if (m_is_right_pressed) {
		arc->rotate(e->pos().x(), e->pos().y());

		render();
	}
}

void OpenGLWindow::mousePressEvent(QMouseEvent* e) {
	if (e->buttons() & Qt::RightButton) {
		qDebug() << "Right mouse pressed";
		m_is_right_pressed = true;

		arc->setStart(e->pos().x(), e->pos().y());
	}
}

void OpenGLWindow::mouseReleaseEvent(QMouseEvent* e) {
	if (e->button() == Qt::RightButton) {
		qDebug() << "Right mouse Released";
		m_is_right_pressed = false;
	}
}

/* OpenGL Events */
void OpenGLWindow::exposeEvent(QExposeEvent *e) {
	Q_UNUSED(e);

	QSize size = this->size();
	glViewport(0, 0, size.width(), size.height());

	render();
}
void OpenGLWindow::resizeEvent(QResizeEvent *e) {
	QSize size = e->size();
	qDebug() << size;

	if (glInitialized) {
		// Resize target texture and framebuffer object
		_initializeTargetTexture(size.width(), size.height());
		_initializeFramebuffer(size.width(), size.height());

		// Resize Arcball
		arc->setBounds(size.width(), size.height());
	}
}